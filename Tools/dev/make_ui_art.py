#!/usr/bin/env python3
"""Generate the map-selection UI images the mod directory does not ship.

ui_startserver.c asks for three images by name. Two of them are reachable:

  interface/art/maps_selected   focuspic, drawn over a thumbnail when its map
                                is selected, at the thumbnail's full width and
                                its height plus the 28px name strip
  interface/art/unknownmap      errorpic, substituted when a map has no
                                levelshot of its own

The third, interface/art/frame1_r, is configured on s_startserver.framer but
that item is never passed to Menu_AddItem, so nothing draws it. It is not
generated here.

Neither image is art in any meaningful sense - a rectangular selection border
and a placeholder tile - so they are described here rather than authored, for
the same reason the aura mesh is: the values are exact and a generator is easier
to adjust than a binary.

Both are RGBA so they compose over what is already on screen; the 2D shader the
engine builds for a bare image blends on alpha.

usage:
    make_ui_art.py <output-directory>

Writes maps_selected.png and unknownmap.png into that directory.
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
    args = ap.parse_args()

    if args.thickness < 1:
        sys.exit("--thickness must be at least 1")
    if args.glow < 1:
        sys.exit("--glow must be at least 1")

    os.makedirs(args.outdir, exist_ok=True)

    # The highlight covers the thumbnail and its name strip, which is how
    # ui_startserver.c sizes the draw.
    sel = os.path.join(args.outdir, "maps_selected.png")
    write_png(sel, args.width, args.height + args.strip,
              selection_border(args.width, args.height + args.strip,
                               args.thickness, args.glow, (255, 190, 90)))

    unk = os.path.join(args.outdir, "unknownmap.png")
    write_png(unk, args.width, args.height, unknown_tile(args.width, args.height))

    print("wrote %s and %s" % (sel, unk))


if __name__ == "__main__":
    main()
