#!/usr/bin/env python3
"""Unwrap the reference art into the band strip the aura shaders sample.

The ring mesh carries the reference's outline r(theta), baked by
make_aura_mesh.py, and hangs its band from INNER_HUG * r to r. This samples
the reference's own field along exactly that parameterization: column u is the
outline's cumulative arc (the mesh's texture coordinate), row t runs from
the inner ring to the outline. A fragment stage that samples this strip at
(u, t) therefore reconstructs the reference field almost by construction -
the mesh supplies the shape, the strip supplies every value inside it.

The strip stores the reference's luminance in alpha with RGB solid white:
the shaders' contract keeps colour in the entity and coverage in alpha, and
on the black-shot reference luminance IS the visible truth.

Writes a PNG for the game data and a raw RGBA blob (width, height as u32le,
then pixels) for aurarender.c, which has a zlib but not a PNG reader.

usage:
    aura_band_from_reference.py <reference.png> <out.png> <out.raw>
                                [--width 1024] [--height 256]
                                [--inner-hug 0.40] [--segments 256]
"""

import argparse
import math
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from png_sheet import decode_png
from make_aura_mesh import read_outline

def bilinear(px, w, h, chan, x, y):
    if x < 0 or y < 0 or x > w - 1 or y > h - 1:
        return 0.0
    x0, y0 = int(x), int(y)
    x1, y1 = min(x0 + 1, w - 1), min(y0 + 1, h - 1)
    fx, fy = x - x0, y - y0
    return ((px[y0][x0][chan] * (1 - fx) + px[y0][x1][chan] * fx) * (1 - fy)
          + (px[y1][x0][chan] * (1 - fx) + px[y1][x1][chan] * fx) * fy) / 255.0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("reference")
    ap.add_argument("out_png")
    ap.add_argument("out_raw")
    ap.add_argument("--width", type=int, default=1024)
    ap.add_argument("--height", type=int, default=256)
    ap.add_argument("--inner-hug", type=float, default=0.40,
                    help="where the band starts, as a fraction of the "
                         "outline; must match INNER_HUG in aura_vp.glsl")
    ap.add_argument("--segments", type=int, default=256,
                    help="outline samples; must match the mesh bake")
    args = ap.parse_args()

    w, h, px = decode_png(args.reference)

    # Same channel decision as the outline bake: alpha when it varies,
    # luminance for art shot on black.
    alo = ahi = px[0][0][3]
    for y in range(0, h, 8):
        for x in range(0, w, 8):
            a = px[y][x][3]
            alo = a if a < alo else alo
            ahi = a if a > ahi else ahi
    chan = 3 if ahi - alo > 32 else 0

    # The outline and its arc table, exactly as the mesh bake computes them,
    # plus the centre the rays were cast from.
    rads, arcs = read_outline(args.reference, args.segments)

    tot = cx = cy = 0.0
    for y in range(0, h, 2):
        for x in range(0, w, 2):
            a = px[y][x][chan]
            if a:
                tot += a
                cx += x * a
                cy += y * a
    cx /= tot
    cy /= tot

    # The reference's outline in pixels, angle by angle, for ray lengths.
    # read_outline normalised its radii; recover the pixel scale the same
    # way it measured: outermost coverage per angle.
    reach = int(math.hypot(max(cx, w - cx), max(cy, h - cy))) + 1
    rpix = []
    for i in range(args.segments):
        th = 2.0 * math.pi * i / args.segments
        dx, dy = math.cos(th), -math.sin(th)
        hi = 0
        for r in range(reach, 0, -1):
            X, Y = int(cx + dx * r), int(cy + dy * r)
            if 0 <= X < w and 0 <= Y < h and px[Y][X][chan] >= 112:
                hi = r
                break
        rpix.append(float(hi))
    # No smoothing, matching the bake: the ray lengths must agree with the
    # mesh's outline or the band's rows sample the wrong radii.

    # Column u -> angle, by inverting the arc table.
    inv = []
    seg = 0
    for c in range(args.width):
        u = (c + 0.5) / args.width
        while seg < args.segments - 1 and arcs[seg + 1] < u:
            seg += 1
        span = arcs[seg + 1] - arcs[seg]
        f = (u - arcs[seg]) / span if span > 0 else 0.0
        inv.append((seg + f) / args.segments * 2.0 * math.pi)

    rows = []
    for j in range(args.height):
        t = (j + 0.5) / args.height
        row = bytearray()
        for c in range(args.width):
            th = inv[c]
            # nearest outline sample for the ray length
            fi = th / (2.0 * math.pi) * args.segments
            i0 = int(fi) % args.segments
            i1 = (i0 + 1) % args.segments
            fr = fi - int(fi)
            rb = rpix[i0] * (1 - fr) + rpix[i1] * fr
            r = rb * (args.inner_hug + (1.0 - args.inner_hug) * t)
            X = cx + math.cos(th) * r
            Y = cy - math.sin(th) * r
            v = bilinear(px, w, h, chan, X, Y)
            row += bytes((255, 255, 255, max(0, min(255, int(round(v * 255))))))
        rows.append(bytes(row))

    raw = b""
    for row in rows:
        raw += b"\x00" + row

    def chunk(t_, d):
        c = t_ + d
        return struct.pack(">I", len(d)) + c + struct.pack(">I", zlib.crc32(c) & 0xffffffff)

    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", args.width, args.height, 8, 6, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 9))
           + chunk(b"IEND", b""))
    with open(args.out_png, "wb") as fh:
        fh.write(png)

    with open(args.out_raw, "wb") as fh:
        fh.write(struct.pack("<II", args.width, args.height))
        for row in rows:
            fh.write(row)

    print("%s + %s: %dx%d band from %s" %
          (args.out_png, args.out_raw, args.width, args.height, args.reference))


if __name__ == "__main__":
    main()
