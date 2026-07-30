#!/usr/bin/env python3
"""Convert an uncompressed ioquake3 TGA screenshot to PNG, with optional stats.

The engine writes 24bpp uncompressed (type 2) TGAs with a bottom-left origin.
Only the standard library is used so this works on a bare macOS/Linux box.

usage:
    tga2png.py <in.tga> [out.png] [--stats] [--quiet]

If out.png is omitted the input path is reused with a .png suffix.
Exit status is 0 on success, 1 on a malformed/unsupported file.
"""
import collections
import struct
import sys
import zlib


def read_tga(path):
    with open(path, "rb") as fh:
        d = fh.read()
    if len(d) < 18:
        raise ValueError("file shorter than a TGA header")
    idlen, cmtype, imgtype = d[0], d[1], d[2]
    width, height = struct.unpack("<HH", d[12:16])
    bpp, desc = d[16], d[17]
    if imgtype != 2:
        raise ValueError("expected uncompressed truecolour (type 2), got type %d" % imgtype)
    if bpp not in (24, 32):
        raise ValueError("expected 24 or 32 bpp, got %d" % bpp)
    if cmtype != 0:
        raise ValueError("colour-mapped TGAs are not supported")
    stride = bpp // 8
    off = 18 + idlen
    need = width * height * stride
    px = d[off:off + need]
    if len(px) < need:
        raise ValueError("truncated pixel data (%d of %d bytes)" % (len(px), need))
    # TGA stores BGR(A); bit 5 of desc set means the origin is already top-left.
    rows = []
    for y in range(height):
        base = y * width * stride
        row = bytearray()
        for x in range(width):
            i = base + x * stride
            row += bytes((px[i + 2], px[i + 1], px[i]))
        rows.append(bytes(row))
    if not (desc & 0x20):
        rows.reverse()
    return width, height, rows


def write_png(path, width, height, rows):
    raw = b"".join(b"\x00" + r for r in rows)

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 6))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as fh:
        fh.write(png)


def stats(width, height, rows):
    counter = collections.Counter()
    for row in rows:
        for x in range(width):
            counter[row[x * 3:x * 3 + 3]] += 1
    total = width * height
    grey = sum(n for px, n in counter.items() if px[0] == px[1] == px[2])
    print("  distinct colours : %d" % len(counter))
    print("  greyscale pixels : %d/%d (%.1f%%)" % (grey, total, 100.0 * grey / total))
    for px, n in counter.most_common(5):
        print("  %5.1f%%  #%02x%02x%02x" % (100.0 * n / total, px[0], px[1], px[2]))


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    flags = {a for a in argv[1:] if a.startswith("--")}
    if not args:
        print(__doc__.strip(), file=sys.stderr)
        return 1
    src = args[0]
    dst = args[1] if len(args) > 1 else (src.rsplit(".", 1)[0] + ".png")
    try:
        width, height, rows = read_tga(src)
    except (OSError, ValueError) as exc:
        print("tga2png: %s: %s" % (src, exc), file=sys.stderr)
        return 1
    write_png(dst, width, height, rows)
    if "--quiet" not in flags:
        print("%s -> %s (%dx%d)" % (src, dst, width, height))
    if "--stats" in flags:
        stats(width, height, rows)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
