#!/usr/bin/env python3
"""Unwrap the reference art into the band strip the aura shaders sample.

The ring mesh carries the reference's outline r(theta), baked by
make_aura_mesh.py, and hangs its band from INNER_HUG * r to r. This samples
the reference's own field along exactly that parameterization: column u is the
outline's cumulative arc (the mesh's texture coordinate), row t runs from
the inner ring to the outline. A fragment stage that samples this strip at
(u, t) therefore reconstructs the reference field almost by construction -
the mesh supplies the shape, the strip supplies every value inside it.

The strip stores the art's own colour over black in RGB and its coverage in
alpha. A greyscale reference bakes RGB equal to its coverage, which the
fragment stage multiplies exactly where it once multiplied solid white - so
colourless art keeps the old behaviour bit for bit, and a coloured reference
carries its own gradient, with auraColor as a multiplier on top.

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
from png_sheet import decode_png, bake_luminance_alpha
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
    ap.add_argument("--frames", type=int, default=1,
                    help="flipbook frames stacked vertically; frame 0 is the "
                         "reference exactly, the rest are lick-jittered "
                         "variants of it; must match STRIP_FRAMES in "
                         "aura_fp.glsl (total height stays power-of-two "
                         "friendly: height * frames)")
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
    # On-black art gets its luminance folded into the flat alpha slot, so
    # every reader below uses channel 3 for brightness while the colour
    # channels stay what the art painted.
    on_black = ahi - alo <= 32
    if on_black:
        bake_luminance_alpha(px, w, h)
    chan = 3

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

    # Below this fraction of the outline the rows blend toward their angular
    # mean: every column converges on the same few pixels at the centre, and
    # sampling them per angle magnifies that handful into wedge spokes and a
    # hard blob. A polar field has no angular information at its centre, so
    # the mean IS the field there.
    CENTRE_BLEND = 0.10

    # Each sample is (r, g, b, a): the art's own colour over black, and the
    # coverage the blend attenuates by. A greyscale reference bakes rgb equal
    # to its luminance, which the fragment stage multiplies exactly where it
    # used to multiply white - so colourless art reproduces the old maths
    # bit for bit, and colour appears only when the reference carries it.
    def sample(X, Y):
        r = bilinear(px, w, h, 0, X, Y)
        g = bilinear(px, w, h, 1, X, Y)
        b = bilinear(px, w, h, 2, X, Y)
        if not on_black:
            # Real alpha: composite the colour over black, coverage is the
            # art's own alpha.
            a = bilinear(px, w, h, 3, X, Y)
            return (r * a, g * a, b * a, a)
        # On-black art is already "over black". Coverage is the brightest
        # channel, not the mean: a saturated flame is opaque where it is
        # vivid, and the mean read solid orange as two-thirds transparent,
        # letting the scene bleed through. Equal channels - every greyscale
        # reference - are untouched either way.
        return (r, g, b, max(r, g, b))

    base = []
    for j in range(args.height):
        t = (j + 0.5) / args.height
        vals = []
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
            vals.append(sample(X, Y))
        frac = args.inner_hug + (1.0 - args.inner_hug) * t
        if frac < CENTRE_BLEND:
            means = tuple(sum(v[k] for v in vals) / len(vals) for k in range(4))
            keep = frac / CENTRE_BLEND
            vals = [tuple(means[k] + (v[k] - means[k]) * keep for k in range(4))
                    for v in vals]
        base.append(vals)

    def band_sample(fu, ft):
        """Bilinear read of the base band (all four channels); u wraps, t clamps."""
        H, W = args.height, args.width
        fu = (fu % 1.0) * W - 0.5
        ft = min(max(ft * H - 0.5, 0.0), H - 1.0)
        c0 = int(math.floor(fu)) % W
        c1 = (c0 + 1) % W
        fru = fu - math.floor(fu)
        r0 = int(ft)
        r1 = min(r0 + 1, H - 1)
        frt = ft - r0
        a = base[r0][c0]; b = base[r0][c1]
        c = base[r1][c0]; d = base[r1][c1]
        return tuple((a[k] * (1 - fru) + b[k] * fru) * (1 - frt)
                   + (c[k] * (1 - fru) + d[k] * fru) * frt for k in range(4))

    # The flipbook variants are the same band with the licks nudged: a smooth
    # periodic displacement in u and a length jitter in t, both scaled by t so
    # the interior veil holds still and only the flame moves. The anime does
    # exactly this - two or three drawings of the same flame, licks redrawn in
    # slightly different places - and deriving the variants from the band
    # keeps every frame the reference's own field.
    SWAY_U = 0.02       # turns, at the tips
    JITTER_T = 0.08     # fraction of lick length, at the tips
    frames = [base]
    for k in range(1, max(args.frames, 1)):
        du = []
        dt = []
        for c in range(args.width):
            x = (c + 0.5) / args.width * 2.0 * math.pi
            du.append((math.sin(3 * x + 1.7 * k)
                       + 0.6 * math.sin(7 * x + 0.9 * k * k + 2.1)
                       + 0.4 * math.sin(13 * x + 2.6 * k)) * SWAY_U / 2.0)
            dt.append((math.sin(5 * x + 2.3 * k + 1.1)
                       + 0.7 * math.sin(11 * x + 1.4 * k)) * JITTER_T / 1.7)
        var = []
        for j in range(args.height):
            t = (j + 0.5) / args.height
            var.append([band_sample((c + 0.5) / args.width + du[c] * t,
                                    t * (1.0 + dt[c] * t))
                        for c in range(args.width)])
        frames.append(var)

    rows = []
    for f in frames:
        for vals in f:
            row = bytearray()
            for v in vals:
                row += bytes(max(0, min(255, int(round(v[k] * 255))))
                             for k in range(4))
            rows.append(bytes(row))

    raw = b""
    for row in rows:
        raw += b"\x00" + row

    def chunk(t_, d):
        c = t_ + d
        return struct.pack(">I", len(d)) + c + struct.pack(">I", zlib.crc32(c) & 0xffffffff)

    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", args.width, args.height * len(frames), 8, 6, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 9))
           + chunk(b"IEND", b""))
    with open(args.out_png, "wb") as fh:
        fh.write(png)

    with open(args.out_raw, "wb") as fh:
        fh.write(struct.pack("<II", args.width, args.height * len(frames)))
        for row in rows:
            fh.write(row)

    print("%s + %s: %dx%d band (%d frame(s)) from %s" %
          (args.out_png, args.out_raw, args.width, args.height * len(frames),
           len(frames), args.reference))


if __name__ == "__main__":
    main()
