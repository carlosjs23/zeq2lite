#!/usr/bin/env python3
"""Bake the training UI's type into glyph atlases.

The training surfaces were drawn with the stock 16x16 bitmap charset, which is
monospaced, unsheared and the same weight everywhere - it cannot say "this word
is the state and that sentence is the instruction". The approved Shockwave
treatment is built out of a heavy condensed display face sheared 7 degrees with
a gold-to-white gradient down the glyphs, plus a compact body face for
sentences, so the type has to come from somewhere other than the charset.

It comes from here, at build time, for the same reason the aura strip and the
HUD gauge do: a generator is easier to re-aim than a binary. Swapping to a
different (or better-licensed) font later is a rerun of this script, not a
redesign - the .def keeps the same shape and cgame reads it the same way.

Deliberately stdlib-only, like every other generator in this directory. That
means the TrueType rasterising below is hand-rolled: `glyf` outlines, quadratic
flattening and a scanline coverage fill. Pillow would be three lines and one
more thing to have installed on a build machine.

Three faces come out:

    display   heavy condensed, sheared 7 deg, gold-to-white baked down the
              glyphs. State words, objectives, headers.
    displayw  the same face and shear in flat white, for the places the deck
              uses the display cut at body size where a gradient would only
              muddy it.
    body      compact upright, flat white. Sentences, labels, tags. Tinted at
              draw time - jade DONE, blue OPEN, grey LOCK are colour multiplies
              on this one atlas.

The gradient is baked because the renderer has no per-glyph gradient stage; the
shear is baked because a sheared quad would need a matrix the 2D path does not
carry. Both are the deck's own build note.

Metrics leave as a COM_Parse-able .def rather than a generated C table, so the
build order stays "compile, then generate art" like everything else here - a
generated header would have to exist before the compile that consumes it.

usage:
    make_training_font.py <output-directory> [--display FONT] [--body FONT]
"""
import argparse
import math
import os
import struct
import sys
import zlib


# ---------------------------------------------------------------- the deck

# Shockwave's numbers, read off the mockup's CSS rather than eyeballed.
SHEAR_DEGREES = 7.0                     # transform:skewX(-7deg) on display type

# linear-gradient(#FFFFFF 28%, #FFD489 60%, #E8A33D 100%), measured down the
# cap-height band because that is what the deck's uppercase display type fills.
GRADIENT = (
    (0.00, 0xFF, 0xFF, 0xFF),
    (0.28, 0xFF, 0xFF, 0xFF),
    (0.60, 0xFF, 0xD4, 0x89),
    (1.00, 0xE8, 0xA3, 0x3D),
)

# Baked at more than any surface asks for: the objective line is ~30px in the
# 640x480 virtual space and a Retina framebuffer doubles that, so a 64px bake is
# still being minified at the largest use. Minifying is free and magnifying is
# the thing that looks broken.
DISPLAY_SIZE = 64
BODY_SIZE = 34

FIRST_CHAR = 32
LAST_CHAR = 126

ATLAS_PAD = 2                           # keeps bilinear taps out of the neighbour


# Freely-licensed condensed faces worth having, in preference order. None of
# these ship with macOS; the search is here so that dropping one into
# ~/Library/Fonts upgrades the atlas on the next build with no code change.
OFL_DISPLAY = (
    "Anton-Regular.ttf", "Anton.ttf",
    "Oswald-Bold.ttf", "Oswald-SemiBold.ttf", "Oswald-Regular.ttf",
    "BebasNeue-Regular.ttf", "LeagueGothic-Regular.ttf",
    "ArchivoNarrow-Bold.ttf", "BarlowCondensed-Bold.ttf",
    "RobotoCondensed-Bold.ttf", "FiraSansCondensed-Bold.ttf",
)
OFL_BODY = (
    "BarlowSemiCondensed-Medium.ttf", "BarlowCondensed-Medium.ttf",
    "Roboto-Medium.ttf", "RobotoCondensed-Regular.ttf",
    "FiraSans-Medium.ttf", "NotoSans-Medium.ttf", "OpenSans-SemiBold.ttf",
)

# What macOS actually has. Proprietary, every one of them - see the caveat this
# script prints when it lands on this list.
SYSTEM_DISPLAY = (
    "/System/Library/Fonts/Supplemental/Impact.ttf",
    "/System/Library/Fonts/Supplemental/DIN Condensed Bold.ttf",
    "/System/Library/Fonts/Supplemental/Arial Narrow Bold.ttf",
)
SYSTEM_BODY = (
    "/System/Library/Fonts/Avenir Next Condensed.ttc#Avenir Next Condensed Demi Bold",
    "/System/Library/Fonts/Supplemental/Arial Narrow Bold.ttf",
    "/System/Library/Fonts/Helvetica.ttc#Helvetica Bold",
)

FONT_DIRS = (
    os.path.expanduser("~/Library/Fonts"),
    "/Library/Fonts",
    "/System/Library/Fonts",
    "/System/Library/Fonts/Supplemental",
)


# ---------------------------------------------------------------- TrueType

class FontError(Exception):
    pass


class TrueType(object):
    """Just enough of an sfnt to get outlines and advances out of it.

    Handles both bare .ttf and .ttc collections; a collection member is named
    with `path#Full Name` and matched against the `name` table, because a
    collection index means nothing to a reader six months later.
    """

    def __init__(self, path, subface=None):
        with open(path, "rb") as fh:
            self.data = fh.read()
        self.path = path
        offsets = self._face_offsets()
        chosen = offsets[0]
        if subface:
            for off in offsets:
                tables = self._read_tables(off)
                if self._full_name(tables) == subface:
                    chosen = off
                    break
            else:
                raise FontError("no face %r in %s" % (subface, path))
        self.tables = self._read_tables(chosen)
        self.name = self._full_name(self.tables) or os.path.basename(path)
        self._read_head()
        self._read_cmap()
        self._read_hmtx()
        self._read_loca()
        self._glyph_cache = {}

    # -- table plumbing

    def _face_offsets(self):
        if self.data[:4] == b"ttcf":
            count = struct.unpack(">I", self.data[8:12])[0]
            return list(struct.unpack(">%dI" % count, self.data[12:12 + 4 * count]))
        return [0]

    def _read_tables(self, base):
        count = struct.unpack(">H", self.data[base + 4:base + 6])[0]
        tables = {}
        for i in range(count):
            rec = base + 12 + 16 * i
            tag = self.data[rec:rec + 4].decode("latin-1")
            off, length = struct.unpack(">II", self.data[rec + 8:rec + 16])
            tables[tag] = (off, length)
        return tables

    def _table(self, tag):
        if tag not in self.tables:
            raise FontError("%s has no %s table" % (self.path, tag))
        off, length = self.tables[tag]
        return self.data[off:off + length]

    def _full_name(self, tables):
        if "name" not in tables:
            return None
        off, _ = tables["name"]
        count, strings = struct.unpack(">HH", self.data[off + 2:off + 6])
        best = None
        for i in range(count):
            rec = off + 6 + 12 * i
            plat, enc, lang, nid, length, soff = struct.unpack(">HHHHHH", self.data[rec:rec + 12])
            if nid != 4:
                continue
            raw = self.data[off + strings + soff:off + strings + soff + length]
            try:
                text = raw.decode("utf-16-be") if plat == 3 else raw.decode("latin-1")
            except UnicodeDecodeError:
                continue
            if plat == 3:
                return text
            best = best or text
        return best

    # -- the tables we need

    def _read_head(self):
        head = self._table("head")
        self.upem = struct.unpack(">H", head[18:20])[0]
        self.loca_long = struct.unpack(">h", head[50:52])[0]
        hhea = self._table("hhea")
        self.ascent = struct.unpack(">h", hhea[4:6])[0]
        self.descent = struct.unpack(">h", hhea[6:8])[0]
        self.num_hmetrics = struct.unpack(">H", hhea[34:36])[0]
        self.num_glyphs = struct.unpack(">H", self._table("maxp")[4:6])[0]
        # Cap height decides where the baked gradient starts and ends. OS/2
        # version 2 carries it; older tables get the usual 0.7em guess.
        self.cap_height = int(round(0.72 * self.upem))
        if "OS/2" in self.tables:
            os2 = self._table("OS/2")
            if struct.unpack(">H", os2[0:2])[0] >= 2 and len(os2) >= 90:
                cap = struct.unpack(">h", os2[88:90])[0]
                if cap > 0:
                    self.cap_height = cap

    def _read_cmap(self):
        cmap = self._table("cmap")
        count = struct.unpack(">H", cmap[2:4])[0]
        best = None
        for i in range(count):
            plat, enc, off = struct.unpack(">HHI", cmap[4 + 8 * i:12 + 8 * i])
            fmt = struct.unpack(">H", cmap[off:off + 2])[0]
            if fmt == 4 and (plat, enc) in ((3, 1), (3, 10), (0, 3), (0, 4)):
                best = off
                break
            if fmt == 4 and best is None:
                best = off
        if best is None:
            raise FontError("%s has no format 4 cmap" % self.path)
        seg2 = struct.unpack(">H", cmap[best + 6:best + 8])[0]
        segs = seg2 // 2
        base = best + 14
        ends = struct.unpack(">%dH" % segs, cmap[base:base + seg2])
        base += seg2 + 2
        starts = struct.unpack(">%dH" % segs, cmap[base:base + seg2])
        base += seg2
        deltas = struct.unpack(">%dh" % segs, cmap[base:base + seg2])
        range_base = base + seg2
        offs = struct.unpack(">%dH" % segs, cmap[range_base:range_base + seg2])
        self._cmap = (cmap, starts, ends, deltas, offs, range_base)

    def glyph_index(self, code):
        cmap, starts, ends, deltas, offs, range_base = self._cmap
        for i in range(len(starts)):
            if code > ends[i] or code < starts[i]:
                continue
            if offs[i] == 0:
                return (code + deltas[i]) & 0xFFFF
            at = range_base + 2 * i + offs[i] + 2 * (code - starts[i])
            gid = struct.unpack(">H", cmap[at:at + 2])[0]
            return 0 if gid == 0 else (gid + deltas[i]) & 0xFFFF
        return 0

    def _read_hmtx(self):
        hmtx = self._table("hmtx")
        self._advances = []
        for i in range(self.num_hmetrics):
            self._advances.append(struct.unpack(">H", hmtx[4 * i:4 * i + 2])[0])
        if not self._advances:
            raise FontError("%s has an empty hmtx" % self.path)

    def advance(self, gid):
        if gid < len(self._advances):
            return self._advances[gid]
        return self._advances[-1]

    def _read_loca(self):
        loca = self._table("loca")
        n = self.num_glyphs + 1
        if self.loca_long:
            self._loca = struct.unpack(">%dI" % n, loca[:4 * n])
        else:
            short = struct.unpack(">%dH" % n, loca[:2 * n])
            self._loca = tuple(v * 2 for v in short)
        self._glyf = self._table("glyf")

    # -- outlines

    def contours(self, gid, depth=0):
        """Font-unit polylines for a glyph, quadratics already flattened."""
        if gid in self._glyph_cache:
            return self._glyph_cache[gid]
        result = []
        if gid + 1 < len(self._loca) and self._loca[gid + 1] > self._loca[gid]:
            blob = self._glyf[self._loca[gid]:self._loca[gid + 1]]
            n = struct.unpack(">h", blob[0:2])[0]
            if n >= 0:
                result = self._simple(blob, n)
            elif depth < 4:
                result = self._composite(blob, depth)
        self._glyph_cache[gid] = result
        return result

    def _simple(self, blob, n):
        ends = struct.unpack(">%dH" % n, blob[10:10 + 2 * n])
        total = (ends[-1] + 1) if n else 0
        at = 10 + 2 * n
        at += 2 + struct.unpack(">H", blob[at:at + 2])[0]
        flags = []
        while len(flags) < total:
            f = blob[at]
            at += 1
            flags.append(f)
            if f & 0x08:
                repeat = blob[at]
                at += 1
                flags.extend([f] * repeat)
        flags = flags[:total]
        xs, value = [], 0
        for f in flags:
            if f & 0x02:
                d = blob[at]
                at += 1
                value += d if f & 0x10 else -d
            elif not f & 0x10:
                value += struct.unpack(">h", blob[at:at + 2])[0]
                at += 2
            xs.append(value)
        ys, value = [], 0
        for f in flags:
            if f & 0x04:
                d = blob[at]
                at += 1
                value += d if f & 0x20 else -d
            elif not f & 0x20:
                value += struct.unpack(">h", blob[at:at + 2])[0]
                at += 2
            ys.append(value)
        contours, start = [], 0
        for end in ends:
            pts = [(xs[i], ys[i], bool(flags[i] & 0x01)) for i in range(start, end + 1)]
            start = end + 1
            if pts:
                contours.append(_flatten(pts))
        return [c for c in contours if len(c) >= 3]

    def _composite(self, blob, depth):
        at = 10
        out = []
        while True:
            flags, gid = struct.unpack(">HH", blob[at:at + 4])
            at += 4
            if flags & 0x0001:
                a1, a2 = struct.unpack(">hh", blob[at:at + 4])
                at += 4
            else:
                a1, a2 = struct.unpack(">bb", blob[at:at + 2])
                at += 2
            xx = yy = 1.0
            xy = yx = 0.0
            if flags & 0x0008:
                xx = yy = _f2dot14(blob, at)
                at += 2
            elif flags & 0x0040:
                xx = _f2dot14(blob, at)
                yy = _f2dot14(blob, at + 2)
                at += 4
            elif flags & 0x0080:
                xx = _f2dot14(blob, at)
                yx = _f2dot14(blob, at + 2)
                xy = _f2dot14(blob, at + 4)
                yy = _f2dot14(blob, at + 6)
                at += 8
            dx, dy = (a1, a2) if flags & 0x0002 else (0, 0)
            for contour in self.contours(gid, depth + 1):
                out.append([(x * xx + y * xy + dx, x * yx + y * yy + dy) for x, y in contour])
            if not flags & 0x0020:
                break
        return out


def _f2dot14(blob, at):
    return struct.unpack(">h", blob[at:at + 2])[0] / 16384.0


def _flatten(points, steps=10):
    """Quadratic contour to a polyline, inserting the implied on-curve points."""
    expanded = []
    count = len(points)
    for i in range(count):
        x, y, on = points[i]
        nx, ny, non = points[(i + 1) % count]
        expanded.append((x, y, on))
        if not on and not non:
            expanded.append(((x + nx) / 2.0, (y + ny) / 2.0, True))
    # Start on an on-curve point so the walk below always has an anchor.
    start = next((i for i, p in enumerate(expanded) if p[2]), None)
    if start is None:
        return []
    expanded = expanded[start:] + expanded[:start]
    out = [(expanded[0][0], expanded[0][1])]
    i = 1
    count = len(expanded)
    while i <= count:
        px, py, on = expanded[i % count]
        if on:
            out.append((px, py))
            i += 1
            continue
        ex, ey, _ = expanded[(i + 1) % count]
        sx, sy = out[-1]
        for s in range(1, steps + 1):
            t = s / float(steps)
            u = 1.0 - t
            out.append((u * u * sx + 2 * u * t * px + t * t * ex,
                        u * u * sy + 2 * u * t * py + t * t * ey))
        i += 2
    return out


# ------------------------------------------------------------- rasterising

def _add_span(cover, row, width, x0, x1, weight):
    if x1 <= x0:
        return
    if x0 < 0.0:
        x0 = 0.0
    if x1 > width:
        x1 = float(width)
    if x1 <= x0:
        return
    first = int(x0)
    last = int(math.ceil(x1)) - 1
    if first == last:
        cover[row + first] += (x1 - x0) * weight
        return
    cover[row + first] += (first + 1 - x0) * weight
    for px in range(first + 1, last):
        cover[row + px] += weight
    cover[row + last] += (x1 - last) * weight


def coverage(contours, width, height, sub=5):
    """Nonzero-winding coverage in [0,1], analytic in x and sampled in y.

    Sampling both axes would need 25 samples for the edge quality a horizontal
    span integral gets for free, and a 7-degree shear puts a near vertical edge
    on every glyph - exactly the case sampling handles worst.
    """
    cover = [0.0] * (width * height)
    edges = []
    for contour in contours:
        n = len(contour)
        for i in range(n):
            x0, y0 = contour[i]
            x1, y1 = contour[(i + 1) % n]
            if y0 == y1:
                continue
            edges.append((y0, y1, x0, (x1 - x0) / (y1 - y0)))
    if not edges:
        return cover
    weight = 1.0 / sub
    for py in range(height):
        row = py * width
        for s in range(sub):
            sy = py + (s + 0.5) * weight
            hits = []
            for y0, y1, x0, slope in edges:
                if (y0 <= sy < y1) or (y1 <= sy < y0):
                    hits.append((x0 + (sy - y0) * slope, 1 if y1 > y0 else -1))
            if not hits:
                continue
            hits.sort()
            winding = 0
            span_start = 0.0
            for x, direction in hits:
                if winding == 0:
                    span_start = x
                winding += direction
                if winding == 0:
                    _add_span(cover, row, width, span_start, x, weight)
    return cover


def gradient_rgb(t):
    if t <= GRADIENT[0][0]:
        return GRADIENT[0][1:]
    for lo, hi in zip(GRADIENT, GRADIENT[1:]):
        if t <= hi[0]:
            span = hi[0] - lo[0]
            f = 0.0 if span <= 0 else (t - lo[0]) / span
            return tuple(int(round(lo[i] + (hi[i] - lo[i]) * f)) for i in (1, 2, 3))
    return GRADIENT[-1][1:]


# ------------------------------------------------------------------- atlas

class Glyph(object):
    __slots__ = ("code", "w", "h", "bx", "by", "advance", "pixels", "x", "y")

    def __init__(self, code, w, h, bx, by, advance, pixels):
        self.code = code
        self.w = w
        self.h = h
        self.bx = bx
        self.by = by
        self.advance = advance
        self.pixels = pixels
        self.x = 0
        self.y = 0


def bake_face(font, size, shear_deg, gradient):
    """Rasterise the printable ASCII range at `size` pixels per em."""
    scale = float(size) / font.upem
    shear = math.tan(math.radians(shear_deg))
    cap = font.cap_height * scale
    glyphs = []
    for code in range(FIRST_CHAR, LAST_CHAR + 1):
        gid = font.glyph_index(code)
        advance = font.advance(gid) * scale
        contours = font.contours(gid)
        placed = []
        for contour in contours:
            pts = []
            for x, y in contour:
                sx = x * scale
                sy = y * scale
                pts.append((sx + shear * sy, sy))
            placed.append(pts)
        if not placed:
            glyphs.append(Glyph(code, 0, 0, 0, 0, advance, None))
            continue
        xs = [p[0] for c in placed for p in c]
        ys = [p[1] for c in placed for p in c]
        x0 = math.floor(min(xs)) - 1
        y1 = math.ceil(max(ys)) + 1
        x1 = math.ceil(max(xs)) + 1
        y0 = math.floor(min(ys)) - 1
        w = int(x1 - x0)
        h = int(y1 - y0)
        # Raster space is y-down with the glyph's top row at 0.
        local = [[(p[0] - x0, y1 - p[1]) for p in c] for c in placed]
        cover = coverage(local, w, h)
        pixels = bytearray(w * h * 4)
        for py in range(h):
            # Where this row sits in the cap band the gradient is measured on.
            t = (y1 - py - 0.5) / cap if cap > 0 else 0.0
            r, g, b = gradient_rgb(1.0 - t) if gradient else (255, 255, 255)
            for px in range(w):
                a = cover[py * w + px]
                if a <= 0.0:
                    continue
                if a > 1.0:
                    a = 1.0
                at = (py * w + px) * 4
                pixels[at] = r
                pixels[at + 1] = g
                pixels[at + 2] = b
                pixels[at + 3] = int(round(a * 255))
        glyphs.append(Glyph(code, w, h, int(x0), int(y1), advance, pixels))
    return glyphs


def pack(glyphs, width):
    """Shelf packer. Tallest first, then by code, so the atlas is stable."""
    order = sorted(glyphs, key=lambda g: (-g.h, g.code))
    x = y = shelf = 0
    for glyph in order:
        if not glyph.pixels:
            continue
        w = glyph.w + ATLAS_PAD
        h = glyph.h + ATLAS_PAD
        if x + w > width:
            x = 0
            y += shelf
            shelf = 0
        glyph.x = x
        glyph.y = y
        x += w
        if h > shelf:
            shelf = h
    height = y + shelf
    size = 1
    while size < height:
        size *= 2
    return size


def compose(glyphs, width, height):
    sheet = bytearray(width * height * 4)
    for glyph in glyphs:
        if not glyph.pixels:
            continue
        for py in range(glyph.h):
            src = py * glyph.w * 4
            dst = ((glyph.y + py) * width + glyph.x) * 4
            sheet[dst:dst + glyph.w * 4] = glyph.pixels[src:src + glyph.w * 4]
    return sheet


def write_png(path, width, height, sheet):
    rows = bytearray()
    stride = width * 4
    for y in range(height):
        rows.append(0)
        rows += sheet[y * stride:(y + 1) * stride]

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    blob = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
            chunk(b"IDAT", zlib.compress(bytes(rows), 9)) + chunk(b"IEND", b""))
    with open(path, "wb") as fh:
        fh.write(blob)
    return len(blob)


# ------------------------------------------------------------------ the run

def find_font(explicit, ofl_names, system_paths):
    """Returns (path, subface, licensed) - `licensed` meaning free to ship."""
    if explicit:
        path, _, sub = explicit.partition("#")
        return path, (sub or None), None
    for name in ofl_names:
        for directory in FONT_DIRS:
            candidate = os.path.join(directory, name)
            if os.path.isfile(candidate):
                return candidate, None, True
    for entry in system_paths:
        path, _, sub = entry.partition("#")
        if os.path.isfile(path):
            return path, (sub or None), False
    raise FontError("no usable font found")


def emit_face(out_dir, lines, tag, image, font, size, glyphs, width, height):
    scale = float(size) / font.upem
    lines.append("face %s" % tag)
    lines.append("\timage %s" % image)
    lines.append("\tsheet %i %i" % (width, height))
    lines.append("\tsize %i" % size)
    lines.append("\tascent %i" % int(round(font.ascent * scale)))
    lines.append("\tdescent %i" % int(round(-font.descent * scale)))
    lines.append("\tcap %i" % int(round(font.cap_height * scale)))
    for glyph in sorted(glyphs, key=lambda g: g.code):
        # code x y w h bearingX bearingY advance, all in atlas pixels at the
        # bake size. bearingY is baseline-up to the top of the cell.
        lines.append("\tglyph %i %i %i %i %i %i %i %i" % (
            glyph.code, glyph.x, glyph.y, glyph.w, glyph.h,
            glyph.bx, glyph.by, int(round(glyph.advance))))
    lines.append("end")


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("output", help="directory to write the atlases into")
    parser.add_argument("--display", help="display face, as path or path#Full Name")
    parser.add_argument("--body", help="body face, as path or path#Full Name")
    parser.add_argument("--def-name", default="training.font")
    args = parser.parse_args(argv[1:])

    os.makedirs(args.output, exist_ok=True)

    disp_path, disp_sub, disp_free = find_font(args.display, OFL_DISPLAY, SYSTEM_DISPLAY)
    body_path, body_sub, body_free = find_font(args.body, OFL_BODY, SYSTEM_BODY)
    display = TrueType(disp_path, disp_sub)
    body = TrueType(body_path, body_sub)

    lines = ["// generated by Tools/dev/make_training_font.py - do not edit",
             "// display: %s" % display.name,
             "// body:    %s" % body.name]

    gold = bake_face(display, DISPLAY_SIZE, SHEAR_DEGREES, True)
    white = bake_face(display, DISPLAY_SIZE, SHEAR_DEGREES, False)
    plain = bake_face(body, BODY_SIZE, 0.0, False)

    written = []
    for tag, image, font, size, glyphs, sheet_w in (
            ("display", "trainingDisplay.png", display, DISPLAY_SIZE, gold, 1024),
            ("displayw", "trainingDisplayW.png", display, DISPLAY_SIZE, white, 1024),
            ("body", "trainingBody.png", body, BODY_SIZE, plain, 512)):
        height = pack(glyphs, sheet_w)
        sheet = compose(glyphs, sheet_w, height)
        path = os.path.join(args.output, image)
        size_bytes = write_png(path, sheet_w, height, sheet)
        emit_face(args.output, lines, tag, "interface/training/" + image,
                  font, size, glyphs, sheet_w, height)
        written.append((image, sheet_w, height, size_bytes))

    def_path = os.path.join(args.output, args.def_name)
    with open(def_path, "w") as fh:
        fh.write("\n".join(lines) + "\n")

    for image, w, h, size_bytes in written:
        print("%s: %ix%i, %i bytes" % (image, w, h, size_bytes))
    print("%s: %i faces, %i glyphs each" % (args.def_name, 3, LAST_CHAR - FIRST_CHAR + 1))
    print("display face: %s (%s)" % (display.name, disp_path))
    print("body face:    %s (%s)" % (body.name, body_path))
    if disp_free is False or body_free is False:
        print("")
        print("LICENSE CAVEAT: one or both faces came from the macOS system fonts,")
        print("which are proprietary and NOT redistributable with the game data. The")
        print("atlases are build products, so shipping needs an OFL/Apache face put")
        print("in ~/Library/Fonts (Anton, Oswald, Barlow Condensed) and this rerun.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
