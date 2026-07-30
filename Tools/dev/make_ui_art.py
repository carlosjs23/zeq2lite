#!/usr/bin/env python3
"""Generate the interface images the mod directory does not ship.

Every one of these is named by the source and absent from the assets, and a
missing image registers as shader handle 0 - the default shader, an opaque black
and white block - so each was drawing a rectangle over the screen rather than
quietly not appearing.

  maps_selected   ui_startserver.c focuspic, over a thumbnail when its map is
                  selected, at the thumbnail's width and its height plus the
                  28px name strip
  unknownmap      ui_startserver.c errorpic, for a map with no levelshot
  loading, ready  cg_info.c, the loading screen's status graphic at 127x64
  connecting,     ui_connect.c, the connect screen's status graphic, drawn over
  searching       cgame's loading screen at the same 127x64
  dots            the progress ellipsis both of those screens place beside it

ui_startserver.c also names interface/art/frame1_r, but that item never reaches
Menu_AddItem, so nothing draws it and nothing generates it.

None of this is art in any meaningful sense - a border, a placeholder tile, and
five words - so it is described here rather than authored, for the same reason
the aura mesh is: a generator is easier to adjust than a binary, and the values
stay visible.

The lettering uses the 5x7 bitmap font below rather than a real typeface. It is
a status caption a few pixels tall on screen; a font dependency would cost more
than it buys, and this keeps the script to the standard library.

Everything is RGBA so it composes over what is already on screen; the 2D shader
the engine builds for a bare image blends on alpha.

usage:
    make_ui_art.py <output-directory>
"""
import argparse
import os
import struct
import sys
import zlib


def write_png(path, width, height, pixel):
    """Emit an 8-bit RGBA PNG. `pixel(x, y)` returns (r, g, b, a)."""
    rows = []
    for y in range(height):
        row = bytearray()
        row.append(0)                      # filter type 0 (None)
        for x in range(width):
            row += bytes(pixel(x, y))
        rows.append(bytes(row))
    raw = b"".join(rows)

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    blob = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))

    with open(path, "wb") as fh:
        fh.write(blob)
    return len(blob)


def selection_border(width, height, thickness, glow, colour):
    """A hollow border: opaque at the edge, fading inward over `glow` pixels.

    Hollow rather than a tinted overlay because this is drawn on top of the
    levelshot - anything with alpha across the middle would wash out the very
    picture the player is trying to look at.
    """
    def pixel(x, y):
        edge = min(x, y, width - 1 - x, height - 1 - y)
        if edge < thickness:
            a = 255
        elif edge < thickness + glow:
            t = (edge - thickness) / float(glow)
            a = int(round(255 * (1.0 - t) ** 2))
        else:
            a = 0
        return (colour[0], colour[1], colour[2], a)
    return pixel


def unknown_tile(width, height):
    """A flat slate with a border and a diagonal hatch.

    Opaque, because it stands in for a missing levelshot rather than sitting
    over one. The hatch is what distinguishes it at a glance from a map whose
    levelshot is simply very dark.
    """
    def pixel(x, y):
        edge = min(x, y, width - 1 - x, height - 1 - y)
        if edge < 2:
            return (150, 150, 160, 255)
        if ((x + y) // 8) % 2 == 0:
            return (48, 48, 56, 255)
        return (38, 38, 45, 255)
    return pixel


# 5x7 uppercase, one string per row, '1' meaning ink. Only the letters the five
# captions need would do, but the full alphabet costs little and lets the next
# caption be added without touching this.
FONT = {
    "A": ("01110", "10001", "10001", "11111", "10001", "10001", "10001"),
    "B": ("11110", "10001", "10001", "11110", "10001", "10001", "11110"),
    "C": ("01110", "10001", "10000", "10000", "10000", "10001", "01110"),
    "D": ("11110", "10001", "10001", "10001", "10001", "10001", "11110"),
    "E": ("11111", "10000", "10000", "11110", "10000", "10000", "11111"),
    "F": ("11111", "10000", "10000", "11110", "10000", "10000", "10000"),
    "G": ("01110", "10001", "10000", "10111", "10001", "10001", "01111"),
    "H": ("10001", "10001", "10001", "11111", "10001", "10001", "10001"),
    "I": ("11111", "00100", "00100", "00100", "00100", "00100", "11111"),
    "J": ("00111", "00010", "00010", "00010", "00010", "10010", "01100"),
    "K": ("10001", "10010", "10100", "11000", "10100", "10010", "10001"),
    "L": ("10000", "10000", "10000", "10000", "10000", "10000", "11111"),
    "M": ("10001", "11011", "10101", "10101", "10001", "10001", "10001"),
    "N": ("10001", "11001", "10101", "10011", "10001", "10001", "10001"),
    "O": ("01110", "10001", "10001", "10001", "10001", "10001", "01110"),
    "P": ("11110", "10001", "10001", "11110", "10000", "10000", "10000"),
    "Q": ("01110", "10001", "10001", "10001", "10101", "10010", "01101"),
    "R": ("11110", "10001", "10001", "11110", "10100", "10010", "10001"),
    "S": ("01111", "10000", "10000", "01110", "00001", "00001", "11110"),
    "T": ("11111", "00100", "00100", "00100", "00100", "00100", "00100"),
    "U": ("10001", "10001", "10001", "10001", "10001", "10001", "01110"),
    "V": ("10001", "10001", "10001", "10001", "10001", "01010", "00100"),
    "W": ("10001", "10001", "10001", "10101", "10101", "11011", "10001"),
    "X": ("10001", "10001", "01010", "00100", "01010", "10001", "10001"),
    "Y": ("10001", "10001", "01010", "00100", "00100", "00100", "00100"),
    "Z": ("11111", "00001", "00010", "00100", "01000", "10000", "11111"),
    "0": ("01110", "10001", "10011", "10101", "11001", "10001", "01110"),
    "1": ("00100", "01100", "00100", "00100", "00100", "00100", "01110"),
    "2": ("01110", "10001", "00001", "00010", "00100", "01000", "11111"),
    "3": ("11111", "00010", "00100", "00010", "00001", "10001", "01110"),
    "4": ("00010", "00110", "01010", "10010", "11111", "00010", "00010"),
    "5": ("11111", "10000", "11110", "00001", "00001", "10001", "01110"),
    "6": ("00110", "01000", "10000", "11110", "10001", "10001", "01110"),
    "7": ("11111", "00001", "00010", "00100", "01000", "01000", "01000"),
    "8": ("01110", "10001", "10001", "01110", "10001", "10001", "01110"),
    "9": ("01110", "10001", "10001", "01111", "00001", "00010", "01100"),
    ".": ("00000", "00000", "00000", "00000", "00000", "01100", "01100"),
    "-": ("00000", "00000", "00000", "11111", "00000", "00000", "00000"),
    "_": ("00000", "00000", "00000", "00000", "00000", "00000", "11111"),
    "=": ("00000", "00000", "11111", "00000", "11111", "00000", "00000"),
    "/": ("00001", "00010", "00010", "00100", "01000", "01000", "10000"),
    " ": ("00000",) * 7,
}

GLYPH_W = 5
GLYPH_H = 7


def caption_scale(width, height, words):
    """One pixel scale that fits every caption in the set.

    Driven by the longest word rather than each in isolation, because these all
    occupy the same rectangle one after another - sizing them independently
    makes the lettering jump as the state changes.
    """
    longest = max(len(w) for w in words)
    cells = longest * (GLYPH_W + 1) - 1
    return max(1, min((width * 8 // 10) // cells, (height * 6 // 10) // GLYPH_H))


def caption(width, height, word, colour, scale, centre=0.30, shadow=(0, 0, 0, 170)):
    """A word set across the canvas at `centre`, over a dropped shadow.

    Sat high rather than centred because the progress dots are drawn inside this
    same rectangle - cg_info.c puts them at 64,52 within the caption's
    0,18,127,64, which is 53% of the way down. A vertically centred word runs
    straight through them.

    The shadow is what keeps this readable: these are drawn on top of the map's
    levelshot, which can be any colour at all.
    """
    word = word.upper()
    glyphs = [FONT[c] for c in word if c in FONT]
    if not glyphs:
        raise ValueError("no renderable characters in %r" % word)

    cells = len(glyphs) * (GLYPH_W + 1) - 1
    ink_w, ink_h = cells * scale, GLYPH_H * scale
    ox = (width - ink_w) // 2
    oy = max(0, int(round(height * centre)) - ink_h // 2)
    drop = max(1, scale // 2)

    def lit(px, py):
        if px < 0 or py < 0:
            return False
        cx, cy = px // scale, py // scale
        if cy >= GLYPH_H:
            return False
        g, col = divmod(cx, GLYPH_W + 1)
        if col == GLYPH_W or g >= len(glyphs):
            return False
        return glyphs[g][cy][col] == "1"

    def pixel(x, y):
        if lit(x - ox, y - oy):
            return colour
        if lit(x - ox - drop, y - oy - drop):
            return shadow
        return (0, 0, 0, 0)
    return pixel


def ellipsis(width, height, count, colour, lit=None):
    """The progress dots, drawn after a caption at 8x4 on screen.

    `lit` is how many of the `count` dots are shown, which is what makes the
    animation: one frame per step, cycled by the shader's animMap.
    """
    if lit is None:
        lit = count

    def pixel(x, y):
        span = width // count
        slot = x // span
        if slot >= lit:
            return (0, 0, 0, 0)
        cx, cy = span // 2 + slot * span, height // 2
        r = min(span, height) // 2 - 1
        if r < 1:
            r = 1
        return colour if (x - cx) ** 2 + (y - cy) ** 2 <= r * r else (0, 0, 0, 0)
    return pixel


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("outdir")
    ap.add_argument("--width", type=int, default=128,
                    help="thumbnail width in menu units (default: 128)")
    ap.add_argument("--height", type=int, default=96,
                    help="thumbnail height, before the name strip (default: 96)")
    ap.add_argument("--strip", type=int, default=28,
                    help="height of the map name strip below it (default: 28)")
    ap.add_argument("--thickness", type=int, default=2,
                    help="solid border thickness in pixels (default: 2)")
    ap.add_argument("--glow", type=int, default=5,
                    help="pixels the border fades inward over (default: 5)")
    ap.add_argument("--caption-centre", type=float, default=0.30,
                    help="where the caption sits down the image, 0..1 "
                         "(default: 0.30, clear of the progress dots)")
    ap.add_argument("--ink", default="240,240,245",
                    help="caption and border colour as 'R,G,B' (default: 240,240,245)")
    args = ap.parse_args()

    if args.thickness < 1:
        sys.exit("--thickness must be at least 1")
    if args.glow < 1:
        sys.exit("--glow must be at least 1")

    try:
        ink = tuple(int(v) for v in args.ink.split(","))
    except ValueError:
        ink = ()
    if len(ink) != 3:
        sys.exit("--ink wants 'R,G,B'")

    os.makedirs(args.outdir, exist_ok=True)

    # The highlight covers the thumbnail and its name strip, which is how
    # ui_startserver.c sizes the draw.
    sel = os.path.join(args.outdir, "maps_selected.png")
    write_png(sel, args.width, args.height + args.strip,
              selection_border(args.width, args.height + args.strip,
                               args.thickness, args.glow, ink))

    unk = os.path.join(args.outdir, "unknownmap.png")
    write_png(unk, args.width, args.height, unknown_tile(args.width, args.height))

    # Captions are authored at twice the 127x64 they are drawn at, so the
    # lettering stays clean once the 640x480 layout is scaled up to the display.
    written = 2
    captions = (("loading", "LOADING"), ("ready", "READY"),
                ("connecting", "CONNECTING"), ("searching", "SEARCHING"))
    scale = caption_scale(254, 128, [w for _, w in captions])
    for name, word in captions:
        write_png(os.path.join(args.outdir, name + ".png"), 254, 128,
                  caption(254, 128, word, ink + (255,), scale,
                          args.caption_centre))
        written += 1

    # One frame per step; scripts/interfaceLoading.shader cycles them with
    # animMap, so the ellipsis animates without the drawing code knowing.
    for step in range(4):
        write_png(os.path.join(args.outdir, "dots%d.png" % step), 48, 16,
                  ellipsis(48, 16, 3, ink + (255,), lit=step))
        written += 1

    print("wrote %d images into %s" % (written, args.outdir))


if __name__ == "__main__":
    main()
