#!/usr/bin/env python3
"""Composite PNGs onto one sheet so they can be looked at.

Game art is mostly transparent - captions, selection borders, the aura's spike
strip - and opening those individually shows a checkerboard or, worse, a viewer's
own idea of a backdrop. What matters when checking generated art is how it reads
against something: does the lettering hold up over a bright levelshot, does the
selection border leave the middle clear, do the spikes fade or cut off square.

So this flattens a set of images onto a chosen background and writes a single
PNG. Deterministic: same inputs, same bytes out, no timestamps and nothing
sampled.

Decoding is done here rather than with an image library because the rest of
Tools/dev is standard-library only and this is not worth breaking that for. It
covers what this tree produces and ships: 8-bit greyscale, RGB, greyscale+alpha,
RGBA and palette, with all five scanline filters.

usage:
    png_sheet.py <out.png> <image> [image ...]
        [--columns N] [--pad N] [--background R,G,B | checker] [--label]

    png_sheet.py /tmp/sheet.png interface/art/*.png --label
    png_sheet.py /tmp/aura.png effects/aura/auraSpikeStrip.png --background 200,60,60
"""
import argparse
import os
import struct
import sys
import zlib

CHECKER = object()


def _paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def decode_png(path):
    """Return (width, height, rows) with rows as lists of (r, g, b, a)."""
    with open(path, "rb") as fh:
        data = fh.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("%s is not a PNG" % path)

    pos, idat, palette, trns = 8, bytearray(), None, None
    width = height = depth = colour = 0
    while pos < len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        tag = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        if tag == b"IHDR":
            width, height, depth, colour, _, _, interlace = struct.unpack(">IIBBBBB", body)
            if depth != 8:
                raise ValueError("%s: only 8-bit channels are supported" % path)
            if interlace:
                raise ValueError("%s: interlaced PNGs are not supported" % path)
        elif tag == b"PLTE":
            palette = body
        elif tag == b"tRNS":
            trns = body
        elif tag == b"IDAT":
            idat += body
        elif tag == b"IEND":
            break
        pos += 12 + length

    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}.get(colour)
    if channels is None:
        raise ValueError("%s: unsupported colour type %d" % (path, colour))

    raw = zlib.decompress(bytes(idat))
    stride = width * channels
    out, prev = [], bytearray(stride)
    pos = 0
    for _ in range(height):
        ftype = raw[pos]
        line = bytearray(raw[pos + 1:pos + 1 + stride])
        pos += 1 + stride
        for i in range(stride):
            a = line[i - channels] if i >= channels else 0
            b = prev[i]
            c = prev[i - channels] if i >= channels else 0
            if ftype == 1:
                line[i] = (line[i] + a) & 0xFF
            elif ftype == 2:
                line[i] = (line[i] + b) & 0xFF
            elif ftype == 3:
                line[i] = (line[i] + ((a + b) >> 1)) & 0xFF
            elif ftype == 4:
                line[i] = (line[i] + _paeth(a, b, c)) & 0xFF
            elif ftype != 0:
                raise ValueError("%s: bad filter type %d" % (path, ftype))
        prev = line

        row = []
        for x in range(width):
            px = line[x * channels:(x + 1) * channels]
            if colour == 0:
                row.append((px[0], px[0], px[0], 255))
            elif colour == 2:
                row.append((px[0], px[1], px[2], 255))
            elif colour == 3:
                i = px[0]
                alpha = trns[i] if trns and i < len(trns) else 255
                row.append((palette[i * 3], palette[i * 3 + 1], palette[i * 3 + 2], alpha))
            elif colour == 4:
                row.append((px[0], px[0], px[0], px[1]))
            else:
                row.append((px[0], px[1], px[2], px[3]))
        out.append(row)
    return width, height, out


def encode_png(path, width, height, pixel_rows):
    rows = []
    for row in pixel_rows:
        line = bytearray()
        line.append(0)
        for px in row:
            line += bytes(px[:3])
        rows.append(bytes(line))

    def chunk(tag, body):
        return (struct.pack(">I", len(body)) + tag + body +
                struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF))

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    blob = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
            chunk(b"IDAT", zlib.compress(b"".join(rows), 9)) + chunk(b"IEND", b""))
    with open(path, "wb") as fh:
        fh.write(blob)
    return len(blob)


def backdrop(x, y, background):
    if background is CHECKER:
        return (90, 90, 96) if ((x // 8) + (y // 8)) % 2 else (60, 60, 66)
    return background


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("output")
    ap.add_argument("images", nargs="+")
    ap.add_argument("--columns", type=int, default=1)
    ap.add_argument("--pad", type=int, default=8, help="gap between cells (default: 8)")
    ap.add_argument("--background", default="25,25,35",
                    help="'R,G,B' or 'checker' (default: 25,25,35)")
    ap.add_argument("--label", action="store_true",
                    help="caption each cell with its filename")
    args = ap.parse_args()

    if args.columns < 1:
        sys.exit("--columns must be at least 1")

    if args.background == "checker":
        background = CHECKER
    else:
        try:
            background = tuple(int(v) for v in args.background.split(","))
        except ValueError:
            background = None
        if not background or len(background) != 3:
            sys.exit("--background wants 'R,G,B' or 'checker'")

    label_h = 0
    font = None
    if args.label:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        try:
            from make_ui_art import FONT, GLYPH_H, GLYPH_W
        except ImportError:
            sys.exit("--label needs make_ui_art.py beside this script")
        font, label_h = (FONT, GLYPH_W, GLYPH_H), GLYPH_H * 2 + 4

    tiles = [(os.path.basename(p),) + decode_png(p) for p in args.images]
    cols = min(args.columns, len(tiles))
    rows_of = [tiles[i:i + cols] for i in range(0, len(tiles), cols)]

    col_w = [0] * cols
    for band in rows_of:
        for i, (_, w, _, _) in enumerate(band):
            col_w[i] = max(col_w[i], w)
    row_h = [max(h for _, _, h, _ in band) + label_h for band in rows_of]

    pad = args.pad
    sheet_w = sum(col_w) + pad * (cols + 1)
    sheet_h = sum(row_h) + pad * (len(rows_of) + 1)
    sheet = [[backdrop(x, y, background) for x in range(sheet_w)] for y in range(sheet_h)]

    oy = pad
    for band, height in zip(rows_of, row_h):
        ox = pad
        for i, (name, w, h, pixels) in enumerate(band):
            for y in range(h):
                for x in range(w):
                    r, g, b, a = pixels[y][x]
                    br, bg_, bb = sheet[oy + y][ox + x]
                    sheet[oy + y][ox + x] = ((r * a + br * (255 - a)) // 255,
                                             (g * a + bg_ * (255 - a)) // 255,
                                             (b * a + bb * (255 - a)) // 255)
            if font:
                glyphs, gw, gh = font
                text = os.path.splitext(name)[0].upper()
                for ci, ch in enumerate(text):
                    bitmap = glyphs.get(ch)
                    if not bitmap:
                        continue
                    for gy in range(gh):
                        for gx in range(gw):
                            if bitmap[gy][gx] != "1":
                                continue
                            for sy in range(2):
                                for sx in range(2):
                                    px = ox + ci * (gw + 1) * 2 + gx * 2 + sx
                                    py = oy + h + 3 + gy * 2 + sy
                                    if px < sheet_w and py < sheet_h:
                                        sheet[py][px] = (235, 235, 235)
            ox += col_w[i] + pad
        oy += height + pad

    size = encode_png(args.output, sheet_w, sheet_h, sheet)
    print("wrote %s (%dx%d, %d bytes, %d image(s))"
          % (args.output, sheet_w, sheet_h, size, len(tiles)))


if __name__ == "__main__":
    main()
