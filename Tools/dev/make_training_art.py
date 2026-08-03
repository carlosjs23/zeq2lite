#!/usr/bin/env python3
"""Generate the training UI's Shockwave support art.

Everything the approved treatment puts behind the type: the tracker's sheared
plate, the gold toast sash and its jade completion twin, the additive charge bar
and its white tip, the segment the readout gauge repeats, and the one triangular
cap that gives every variable-width slab its 20-degree cut.

Two shear angles, and they are not the same number by accident. Type leans 7
degrees, geometry cuts at 20 - the deck's own CSS uses skewX(-7deg) on the
glyphs and clip-paths around tan(20) on every panel edge. A slab cut at the type
angle looks like a mistake next to the type; a glyph sheared to the panel angle
falls over.

The cut is baked into the art rather than applied at draw time because the 2D
path is trap_R_DrawStretchPic and nothing else: axis-aligned quads with no
matrix to shear them. Fixed-size pieces (plate, sash, bar) are baked whole;
anything whose width follows its text is drawn as cap + stretched middle + cap,
so the cap is a right triangle that keeps its angle only when it is scaled to
tan(20) * height. CG_DrawShearedSlab in cg_text.c is what enforces that.

Also writes scripts/interfaceTraining.shader, because the charge bar and the lit
segment are additive and a bare .png registers as an alpha blend. The scripts
directory is game data, so the shader is a build product exactly like the images
it names.

usage:
    make_training_art.py <mod-directory>
"""
import argparse
import math
import os
import struct
import sys
import zlib


SHEAR = math.tan(math.radians(20.0))        # every panel edge in the deck

# Sampled straight out of the mockup's CSS.
KI = (0xE8, 0xA3, 0x3D)                     # --ki, the gold
GAUGE = (0x4F, 0xA3, 0xE3)                  # --gauge, the HUD blue
DONE = (0x79, 0xD6, 0xA6)                   # --done, completion jade
BAR_RAMP = (                                # the charge fill, left to right
    (0.00, 0x2C, 0x7F, 0xD0),
    (0.60, 0x4F, 0xA3, 0xE3),
    (1.00, 0xBF, 0xE6, 0xFF),
)
SASH_RAMP = (                               # rgba(232,163,61,.94) -> burnt -> out
    (0.00, 0xE8, 0xA3, 0x3D, 0.94),
    (0.64, 0xB0, 0x5C, 0x12, 0.60),
    (1.00, 0xB0, 0x5C, 0x12, 0.00),
)
SASH_DONE_RAMP = (
    (0.00, 0x79, 0xD6, 0xA6, 0.92),
    (0.64, 0x10, 0x4C, 0x34, 0.72),
    (1.00, 0x07, 0x27, 0x1A, 0.00),
)
PLATE_RAMP = (                              # the tracker plate, left to right
    (0.00, 0x0A, 0x0E, 0x1A, 0.00),
    (0.20, 0x0A, 0x10, 0x1E, 0.74),
    (1.00, 0x0C, 0x14, 0x26, 0.90),
)

SUPERSAMPLE = 4


def write_png(path, width, height, pixels):
    rows = bytearray()
    stride = width * 4
    for y in range(height):
        rows.append(0)
        rows += pixels[y * stride:(y + 1) * stride]

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    blob = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
            chunk(b"IDAT", zlib.compress(bytes(rows), 9)) + chunk(b"IEND", b""))
    with open(path, "wb") as fh:
        fh.write(blob)
    return len(blob)


def ramp(stops, t):
    if t <= stops[0][0]:
        return stops[0][1:]
    for lo, hi in zip(stops, stops[1:]):
        if t <= hi[0]:
            span = hi[0] - lo[0]
            f = 0.0 if span <= 0 else (t - lo[0]) / span
            return tuple(lo[i] + (hi[i] - lo[i]) * f for i in range(1, len(lo)))
    return stops[-1][1:]


def render(width, height, shade):
    """Supersample `shade(u, v) -> (r, g, b, a)` with u,v in [0,1]."""
    out = bytearray(width * height * 4)
    step = 1.0 / SUPERSAMPLE
    weight = 1.0 / (SUPERSAMPLE * SUPERSAMPLE)
    for y in range(height):
        for x in range(width):
            r = g = b = a = 0.0
            for sy in range(SUPERSAMPLE):
                v = (y + (sy + 0.5) * step) / height
                for sx in range(SUPERSAMPLE):
                    u = (x + (sx + 0.5) * step) / width
                    sr, sg, sb, sa = shade(u, v)
                    # Premultiplied accumulation, undone below: averaging a
                    # colour where the sample is transparent drags the edge
                    # toward whatever that transparent colour happened to be.
                    r += sr * sa
                    g += sg * sa
                    b += sb * sa
                    a += sa
            at = (y * width + x) * 4
            if a <= 0.0:
                continue
            out[at] = _clamp(r / a)
            out[at + 1] = _clamp(g / a)
            out[at + 2] = _clamp(b / a)
            out[at + 3] = _clamp(a * weight * 255.0)
    return out


def _clamp(value):
    value = int(round(value))
    return 0 if value < 0 else (255 if value > 255 else value)


# ------------------------------------------------------------------ pieces

def shear_cap(size=64):
    """A right triangle: opaque below-right of the cut, clear above-left.

    Stretched to (tan(20) * height) x height it becomes the left end of any
    slab; drawn with its s coordinates reversed it becomes the right end. The
    diagonal is the only thing in it, so it is the only thing resolved.
    """
    def shade(u, v):
        return (255.0, 255.0, 255.0, 1.0 if u >= 1.0 - v else 0.0)
    return size, size, render(size, size, shade)


def plate(width=680, height=172):
    """The tracker's plate: dark to the right, gone to the left, cut on the top
    left corner the way the deck's clip-path cuts it."""
    cut = SHEAR * height / float(width)

    def shade(u, v):
        if u < cut * (1.0 - v):
            return (0.0, 0.0, 0.0, 0.0)
        r, g, b, a = ramp(PLATE_RAMP, u)
        return (r, g, b, a)
    return width, height, render(width, height, shade)


def sash(stops, width=728, height=80):
    """The toast sash: a gold wipe with its trailing edge cut back."""
    cut = 0.052                             # 3cqw of a 58cqw sash

    def shade(u, v):
        if u > 1.0 - cut * v:
            return (0.0, 0.0, 0.0, 0.0)
        r, g, b, a = ramp(stops, u)
        return (r, g, b, a)
    return width, height, render(width, height, shade)


def bar_track(width=512, height=24):
    """The empty charge bar: a sheared trough with a hairline inset border."""
    cut = SHEAR * height / float(width)
    edge = 1.6 / height

    def shade(u, v):
        if u < cut * (1.0 - v) or u > 1.0 - cut * v:
            return (0.0, 0.0, 0.0, 0.0)
        border = v < edge or v > 1.0 - edge
        return (255.0, 255.0, 255.0, 0.30 if border else 0.14)
    return width, height, render(width, height, shade)


def bar_fill(width=512, height=24):
    """The additive charge fill. Only the leading (left) end is cut - the
    trailing end is wherever the fill stops, and the tip covers that."""
    cut = SHEAR * height / float(width)

    def shade(u, v):
        if u < cut * (1.0 - v):
            return (0.0, 0.0, 0.0, 0.0)
        r, g, b = ramp(BAR_RAMP, u)
        # Brighter across the middle of the trough than at its lips, so the
        # fill reads as a rod of light rather than as a flat swatch.
        lift = 1.0 - abs(v - 0.5) * 0.7
        return (r * lift, g * lift, b * lift, 1.0)
    return width, height, render(width, height, shade)


def bar_tip(width=48, height=96):
    """The white leading edge, with the overhang the deck gives it above and
    below the trough and a soft additive halo around it."""
    core = 0.18
    over = 0.10                             # the tip stands proud of the bar

    def shade(u, v):
        du = abs(u - 0.5) / core
        if v < over or v > 1.0 - over:
            dv = (over - v) / over if v < over else (v - (1.0 - over)) / over
        else:
            dv = 0.0
        glow = math.exp(-(du * du) * 2.2) * math.exp(-(dv * dv) * 2.0)
        if du <= 1.0 and dv <= 0.0:
            return (255.0, 255.0, 255.0, 1.0)
        return (0xBF, 0xE6, 0xFF, glow * 0.9)
    return width, height, render(width, height, shade)


def segment(width=32, height=24):
    """One tick of the readout gauge: a sheared block, tinted where it is drawn.

    The readout caps at ten of these. Twelve was the honest count for the power
    charge, but at readout scale the gaps eat the fill and it stops reading as a
    quantity at all - the deviation is recorded in the deck.
    """
    cut = SHEAR * height / float(width)

    def shade(u, v):
        if u < cut * (1.0 - v) or u > 1.0 - cut * v:
            return (0.0, 0.0, 0.0, 0.0)
        lift = 1.0 - abs(v - 0.5) * 0.55
        return (255.0 * lift, 255.0 * lift, 255.0 * lift, 1.0)
    return width, height, render(width, height, shade)


def _diamond(u, v):
    """Chebyshev-rotated distance from the middle: 0 at the centre, 1 on the rim."""
    return abs(u - 0.5) * 2.0 + abs(v - 0.5) * 2.0


def radar_master(size=64):
    """The quiet waypoint: a hollow lozenge, one master standing somewhere.

    Drawn white and tinted by trap_R_SetColor, the way every interface/sense
    blip is - a bare .png registers as a 2D shader with rgbGen vertex, so the
    colour is the caller's and this file only decides the shape.

    A lozenge rather than a disc because the ki-sense blips are round: at
    radar scale the eye separates silhouettes long before it separates hues,
    and a player must never mistake a master for a fighter.
    """
    # Both edges of the ring are feathered over a fixed band so the outline
    # survives being scaled down to eighteen virtual units.
    inner, outer, soft = 0.56, 0.88, 0.10

    def shade(u, v):
        d = _diamond(u, v)
        a = min((d - inner) / soft, (outer - d) / soft, 1.0)
        return (255.0, 255.0, 255.0, max(a, 0.0))
    return size, size, render(size, size, shade)


def radar_quest(size=64):
    """The destination: the same lozenge, ringed and cored.

    Three elements so it reads as "this one" at a glance and still reads as a
    master: the outline the quiet mark has, a bright core inside it, and an
    outer ring that the pulse in cg_radar.c breathes.
    """
    # The outer ring has to close inside the cell: a diamond of distance 1.0
    # already touches the edge midpoints, so anything past that is clipped
    # square and the mark stops being a diamond.
    inner, outer, soft = 0.34, 0.52, 0.08
    ring_in, ring_out = 0.78, 0.96

    def shade(u, v):
        d = _diamond(u, v)
        core = 1.0 - d / inner
        band = min((d - inner) / soft, (outer - d) / soft, 1.0)
        ring = min((d - ring_in) / soft, (ring_out - d) / soft, 1.0)
        a = max(core * 0.85, band, ring * 0.8, 0.0)
        return (255.0, 255.0, 255.0, min(a, 1.0))
    return size, size, render(size, size, shade)


SHADER = """// generated by Tools/dev/make_training_art.py - do not edit
//
// The charge fill and its tip are additive because that is the language the
// auras and ki gauges already speak: light adds to the scene instead of
// masking it. rgbGen/alphaGen vertex is what makes trap_R_SetColor reach them -
// rgbGen identity, which the stock HUD shaders use, would ignore the tint and
// the fade both.
trainingBarFill
{
\tnopicmip
\tnomipmaps
\t{
\t\tmap interface/training/barFill.png
\t\tblendfunc GL_SRC_ALPHA GL_ONE
\t\trgbGen vertex
\t\talphaGen vertex
\t}
}
trainingBarTip
{
\tnopicmip
\tnomipmaps
\t{
\t\tmap interface/training/barTip.png
\t\tblendfunc GL_SRC_ALPHA GL_ONE
\t\trgbGen vertex
\t\talphaGen vertex
\t}
}
trainingSegmentLit
{
\tnopicmip
\tnomipmaps
\t{
\t\tmap interface/training/segment.png
\t\tblendfunc GL_SRC_ALPHA GL_ONE
\t\trgbGen vertex
\t\talphaGen vertex
\t}
}
"""


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("mod", help="mod directory, e.g. Build/.../ZEQ2")
    args = parser.parse_args(argv[1:])

    art = os.path.join(args.mod, "interface", "training")
    scripts = os.path.join(args.mod, "scripts")
    os.makedirs(art, exist_ok=True)
    os.makedirs(scripts, exist_ok=True)

    pieces = (
        ("shearCap.png", shear_cap()),
        ("plate.png", plate()),
        ("sash.png", sash(SASH_RAMP)),
        ("sashDone.png", sash(SASH_DONE_RAMP)),
        ("barTrack.png", bar_track()),
        ("barFill.png", bar_fill()),
        ("barTip.png", bar_tip()),
        ("segment.png", segment()),
        ("radarMaster.png", radar_master()),
        ("radarQuest.png", radar_quest()),
    )
    for name, (width, height, pixels) in pieces:
        size = write_png(os.path.join(art, name), width, height, pixels)
        print("%s: %ix%i, %i bytes" % (name, width, height, size))

    path = os.path.join(scripts, "interfaceTraining.shader")
    with open(path, "w") as fh:
        fh.write(SHADER)
    print("interfaceTraining.shader: 3 additive shaders")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
