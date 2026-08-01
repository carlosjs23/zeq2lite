#!/usr/bin/env python3
"""Turn a reference image of an aura into the tiling spike strip.

make_aura_texture.py builds the strip from parameters, which means matching a
piece of reference art is a search: change a number, render, look, repeat. When
the reference is a clean shot of the aura on a flat background there is no need
to search at all - the strip can be read straight off it.

The reference is a ring seen head on. The strip wants U running around that ring
and V running across its band, inner to outer, so the conversion is a polar
unwrap about the ring's centre. A full turn maps to the full width, which is
what makes the result tile along U: the last column is the neighbour of the
first by construction rather than by anything being blended.

Two things are deliberately thrown away.

The silhouette. The reference is a teardrop because that is the shape the
reference was drawn at, but in the engine the shape comes from the vertex
program working against the player's bounding box - the strip supplies the
flame, not the outline. So each column is normalised against a *smoothed*
version of the ring's own boundary, which removes the teardrop while leaving
every lick and hair that deviates from it. Pass --keep-shape to skip this and
bake the outline in; useful for seeing what was read, not for shipping.

Colour. The engine's aura art is white with the shape in alpha, and the
fragment program premultiplies by that alpha before adding, so anything but
white here would fight the per-tier auraColor. Only coverage is taken.
"""

import argparse
import math
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from png_sheet import decode_png


def key_alpha(px, key, softness):
    """Coverage at one pixel, 0 on the background and 1 on the aura.

    Distance from the key colour rather than a per-channel test: a chroma
    background bleeds into the semi-transparent edges of whatever was shot
    against it, and those edges are most of what this tool exists to capture.
    A hard test throws them away and leaves the licks looking clipped.
    """
    d = math.sqrt(sum((px[i] - key[i]) ** 2 for i in range(3))) / 441.67
    if d <= 0.0:
        return 0.0
    return min(1.0, d / max(softness, 1e-6))


def sample(alpha, w, h, x, y):
    """Bilinear coverage, zero outside. The unwrap reads along rays that cross
    the pixel grid at every angle, so nearest-neighbour here shows up as steps
    running around the ring."""
    if x < 0 or y < 0 or x > w - 1 or y > h - 1:
        return 0.0
    x0, y0 = int(x), int(y)
    x1, y1 = min(x0 + 1, w - 1), min(y0 + 1, h - 1)
    fx, fy = x - x0, y - y0
    return ((alpha[y0][x0] * (1 - fx) + alpha[y0][x1] * fx) * (1 - fy) +
            (alpha[y1][x0] * (1 - fx) + alpha[y1][x1] * fx) * fy)


def smooth_ring(values, window):
    """Circular moving average. Circular because these are measurements around
    a closed loop, and a linear filter would disagree with itself at the seam -
    which is the one place in the strip where a discontinuity is guaranteed to
    be visible."""
    n = len(values)
    if window <= 1 or n == 0:
        return list(values)
    half = max(int(window) // 2, 1)
    out = []
    for i in range(n):
        total = 0.0
        for k in range(-half, half + 1):
            total += values[(i + k) % n]
        out.append(total / (2 * half + 1))
    return out


def encode_rgba(path, width, height, rows):
    raw = []
    for row in rows:
        line = bytearray()
        line.append(0)
        for a in row:
            line += bytes((255, 255, 255, a))
        raw.append(bytes(line))

    def chunk(tag, body):
        return (struct.pack(">I", len(body)) + tag + body +
                struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF))

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    blob = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
            chunk(b"IDAT", zlib.compress(b"".join(raw), 9)) + chunk(b"IEND", b""))
    with open(path, "wb") as fh:
        fh.write(blob)
    return len(blob)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("reference", help="aura shot against a flat background")
    ap.add_argument("output", help="strip to write (PNG, RGBA)")
    ap.add_argument("--width", type=int, default=512,
                    help="strip width, i.e. angular samples (default: 512)")
    ap.add_argument("--height", type=int, default=256,
                    help="strip height, i.e. radial samples (default: 256)")
    ap.add_argument("--key", default="auto",
                    help="'R,G,B' background colour, or auto to take the most "
                         "common pixel (default: auto)")
    ap.add_argument("--softness", type=float, default=0.35,
                    help="distance from the key at which a pixel counts as "
                         "fully covered (default: 0.35)")
    ap.add_argument("--threshold", type=float, default=0.12,
                    help="coverage a ray must reach to count as the band, when "
                         "measuring the ring's boundary (default: 0.12)")
    ap.add_argument("--smooth", type=int, default=64,
                    help="angular window used to smooth the boundary before "
                         "normalising; larger removes more of the silhouette "
                         "and less of the flame (default: 64)")
    ap.add_argument("--centre", default="auto",
                    help="'x,y' ring centre in pixels, or auto for the "
                         "coverage-weighted centroid")
    ap.add_argument("--keep-shape", action="store_true",
                    help="do not normalise; bake the reference's own outline "
                         "into the strip")
    ap.add_argument("--peak", type=int, default=240,
                    help="alpha the strongest coverage maps to (default: 240)")
    args = ap.parse_args()

    w, h, rows = decode_png(args.reference)

    if args.key == "auto":
        counts = {}
        for row in rows:
            for px in row:
                k = px[:3]
                counts[k] = counts.get(k, 0) + 1
        key = max(counts.items(), key=lambda kv: kv[1])[0]
    else:
        key = tuple(int(v) for v in args.key.split(","))
        if len(key) != 3:
            sys.exit("--key wants R,G,B")

    # Coverage, with any alpha the reference already carried folded in: a PNG
    # cut out beforehand keys to nothing useful otherwise.
    alpha = [[key_alpha(px, key, args.softness) * (px[3] / 255.0)
              for px in row] for row in rows]

    total = sum(sum(r) for r in alpha)
    if total <= 0.0:
        sys.exit("no coverage found - wrong --key? it read %r" % (key,))

    if args.centre == "auto":
        cx = sum(x * a for row in alpha for x, a in enumerate(row)) / total
        cy = sum(y * sum(row) for y, row in enumerate(alpha)) / total
    else:
        parts = args.centre.split(",")
        if len(parts) != 2:
            sys.exit("--centre wants x,y")
        cx, cy = (float(parts[0]), float(parts[1]))

    # How far a ray can travel before it leaves the image, per angle. The band
    # is measured within this.
    reach = int(math.hypot(max(cx, w - cx), max(cy, h - cy))) + 1

    inner, outer = [], []
    for i in range(args.width):
        th = 2.0 * math.pi * (i + 0.5) / args.width
        dx, dy = math.cos(th), math.sin(th)
        lo, hi = None, None
        for r in range(reach):
            if sample(alpha, w, h, cx + dx * r, cy + dy * r) >= args.threshold:
                if lo is None:
                    lo = r
                hi = r
        inner.append(lo if lo is not None else 0.0)
        outer.append(hi if hi is not None else float(reach))

    if args.keep_shape:
        lo_s = [0.0] * args.width
        hi_s = [float(reach)] * args.width
    else:
        lo_s = smooth_ring(inner, args.smooth)
        hi_s = smooth_ring(outer, args.smooth)

    out_rows = []
    for y in range(args.height):
        v = (y + 0.5) / args.height
        row = []
        for i in range(args.width):
            th = 2.0 * math.pi * (i + 0.5) / args.width
            # A little inside the smoothed inner edge, so the innermost rows
            # land in clear space. The ring's inner edge is a closed loop of
            # geometry and any coverage on row zero draws that loop as a hard
            # oval over the character - the same trap envelope() in
            # make_aura_texture.py exists to avoid.
            r0 = lo_s[i] * 0.92
            r = r0 + (hi_s[i] - r0) * v
            a = sample(alpha, w, h, cx + math.cos(th) * r, cy + math.sin(th) * r)
            row.append(int(round(max(0.0, min(1.0, a)) * args.peak)))
        out_rows.append(row)

    # Row zero transparent regardless of what was sampled there, for the reason
    # given above; the reference cannot be trusted to have been drawn with the
    # engine's inner loop in mind.
    out_rows[0] = [0] * args.width

    size = encode_rgba(args.output, args.width, args.height, out_rows)
    covered = sum(1 for row in out_rows for a in row if a > 8)
    print("%s: %dx%d from %dx%d, key %r, centre (%.1f, %.1f), "
          "%.1f%% covered, %d bytes"
          % (args.output, args.width, args.height, w, h, key, cx, cy,
             100.0 * covered / (args.width * args.height), size))


if __name__ == "__main__":
    main()
