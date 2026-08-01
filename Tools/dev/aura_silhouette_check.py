#!/usr/bin/env python3
"""Measure the aura the real pipeline draws against the reference art.

aurarender.c runs the shipped mesh and the shipped GLSL in a headless GL
context and writes the frame's alpha plane; this drives it, then measures
both that mask and the reference the same way: rays from the coverage
centroid, outermost sample past the same threshold, one radius per angle.
The result is two curves in the same units - crown-normalised radius per
angle - so "how far is the shape from the reference" is rms, bias and
tongue statistics, not an impression from a screenshot. No game engine
runs at any point.

usage:
    aura_silhouette_check.py [--mask PATH]      # skip rendering, measure PATH
                             [--reference PATH] [--plot out.png]
                             [--keep-mask PATH]
"""

import argparse
import math
import os
import struct
import subprocess
import sys
import tempfile
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from png_sheet import decode_png

THRESHOLD = 112          # the bar the reference outline was measured at
ANGLES = 512


def render_mask(root, out_png, size=2048):
    """Compile aurarender if needed and run the real pipeline."""
    dev = os.path.join(root, "Tools/dev")
    build = os.path.join(root, "Build/Release-darwin-arm")
    binary = os.path.join(build, "aurarender")
    source = os.path.join(dev, "aurarender.c")
    if (not os.path.exists(binary)
            or os.path.getmtime(binary) < os.path.getmtime(source)):
        subprocess.run(["cc", "-O2", "-o", binary, source, "-lz",
                        "-framework", "OpenGL", "-DGL_SILENCE_DEPRECATION"],
                       check=True)
    cmd = [binary,
           os.path.join(build, "ZEQ2/models/effects/aura.iqm"),
           os.path.join(root, "GameData/glsl/aura_vp.glsl"),
           os.path.join(root, "GameData/glsl/aura_fp.glsl"),
           out_png, str(size)]
    strip = os.path.join(build, "ZEQ2/effects/aura/auraStrip.raw")
    if os.path.exists(strip):
        cmd.append(strip)
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)


def boundary(path):
    """Crown-normalised outermost radius per angle, tip at angle pi/2.

    Works for both inputs: the reference keeps its silhouette in alpha, and
    aurarender writes the pipeline's alpha as greyscale - so measure
    whichever channel carries it: the maximum of the two.
    """
    w, h, px = decode_png(path)

    # The reference keeps its silhouette in alpha; aurarender writes plain
    # greyscale, which decodes with a constant alpha plane. Trust alpha only
    # if it actually varies, otherwise read luminance.
    alo = ahi = px[0][0][3]
    for y in range(0, h, 8):
        for x in range(0, w, 8):
            a = px[y][x][3]
            alo = a if a < alo else alo
            ahi = a if a > ahi else ahi
    chan = 3 if ahi - alo > 32 else 0

    # Anchor at the thresholded mask's bounding-box centre, not the coverage
    # centroid: a centroid moves with the mass distribution, so shortening
    # the base shifts it upward and the measured base radius grows - the
    # numbers move against the geometry and every pole-level comparison
    # scrambles. The box centre only moves when the silhouette itself does.
    x0, x1, y0, y1 = w, -1, h, -1
    for y in range(0, h, 2):
        for x in range(0, w, 2):
            if px[y][x][chan] >= THRESHOLD:
                x0 = x if x < x0 else x0
                x1 = x if x > x1 else x1
                y0 = y if y < y0 else y0
                y1 = y if y > y1 else y1
    if x1 < 0:
        sys.exit("no coverage in %s" % path)
    cx = (x0 + x1) * 0.5
    cy = (y0 + y1) * 0.5

    reach = int(math.hypot(max(cx, w - cx), max(cy, h - cy))) + 1
    rs = []
    for i in range(ANGLES):
        th = 2.0 * math.pi * i / ANGLES
        dx, dy = math.cos(th), -math.sin(th)   # +y up, image y down
        hi = 0
        for r in range(reach, 0, -1):
            x, y = int(cx + dx * r), int(cy + dy * r)
            if 0 <= x < w and 0 <= y < h and px[y][x][chan] >= THRESHOLD:
                hi = r
                break
        rs.append(float(hi))
    peak = max(rs)
    return [r / peak for r in rs]


def width_profile(path, bands=256):
    """Left and right half-widths per height band, in units of the shape's
    own bounding-box height. h = 0 at the silhouette's bottom, 1 at its top;
    widths are measured from the box's vertical centreline."""
    w, h, px = decode_png(path)
    alo = ahi = px[0][0][3]
    for y in range(0, h, 8):
        for x in range(0, w, 8):
            a = px[y][x][3]
            alo = a if a < alo else alo
            ahi = a if a > ahi else ahi
    chan = 3 if ahi - alo > 32 else 0

    x0, x1, y0, y1 = w, -1, h, -1
    for y in range(h):
        for x in range(w):
            if px[y][x][chan] >= THRESHOLD:
                x0 = x if x < x0 else x0
                x1 = x if x > x1 else x1
                y0 = y if y < y0 else y0
                y1 = y if y > y1 else y1
    if x1 < 0:
        sys.exit("no coverage in %s" % path)
    cx = (x0 + x1) * 0.5
    span = float(y1 - y0)

    out = []
    for b in range(bands):
        yy = y1 - (b + 0.5) / bands * span
        y = int(yy)
        left = right = 0.0
        if 0 <= y < h:
            for x in range(x0, x1 + 1):
                if px[y][x][chan] >= THRESHOLD:
                    if x < cx:
                        left = max(left, (cx - x) / span)
                    else:
                        right = max(right, (x - cx) / span)
        out.append((left, right))
    return out


def field_stats(ref_path, gen_path, grid=(192, 240), diff_png=None):
    """Full-field comparison: both images resampled onto a common grid by
    their silhouette bounding boxes, values as fractions of full scale.
    This is the 1:1 test - not just where the boundary sits, but every
    value inside it: interior veil, hot rim, tongue interiors, feather."""
    fields = []
    for path in (ref_path, gen_path):
        w, h, px = decode_png(path)
        alo = ahi = px[0][0][3]
        for y in range(0, h, 8):
            for x in range(0, w, 8):
                a = px[y][x][3]
                alo = a if a < alo else alo
                ahi = a if a > ahi else ahi
        chan = 3 if ahi - alo > 32 else 0
        x0, x1, y0, y1 = w, -1, h, -1
        for y in range(h):
            for x in range(w):
                if px[y][x][chan] >= THRESHOLD:
                    x0 = x if x < x0 else x0
                    x1 = x if x > x1 else x1
                    y0 = y if y < y0 else y0
                    y1 = y if y > y1 else y1
        if x1 < 0:
            sys.exit("no coverage in %s" % path)
        gw, gh = grid
        f = []
        for j in range(gh):
            yy = y0 + (j + 0.5) / gh * (y1 - y0)
            row = []
            for i in range(gw):
                xx = x0 + (i + 0.5) / gw * (x1 - x0)
                row.append(px[int(yy)][int(xx)][chan] / 255.0)
            f.append(row)
        fields.append(f)

    ref, gen = fields
    gw, gh = grid
    n = gw * gh
    diffs = [gen[j][i] - ref[j][i] for j in range(gh) for i in range(gw)]
    mean = sum(abs(d) for d in diffs) / n
    rms = math.sqrt(sum(d * d for d in diffs) / n)
    p95 = sorted(abs(d) for d in diffs)[int(n * 0.95)]

    if diff_png:
        img = [[min(255, int(abs(gen[j][i] - ref[j][i]) * 512))
                for i in range(gw)] for j in range(gh)]
        raw = b""
        for row in img:
            raw += b"\x00" + bytes(row)

        def chunk(t, d):
            c = t + d
            return struct.pack(">I", len(d)) + c + struct.pack(">I", zlib.crc32(c) & 0xffffffff)

        png = (b"\x89PNG\r\n\x1a\n"
               + chunk(b"IHDR", struct.pack(">IIBBBBB", gw, gh, 8, 0, 0, 0, 0))
               + chunk(b"IDAT", zlib.compress(raw, 9))
               + chunk(b"IEND", b""))
        with open(diff_png, "wb") as fh:
            fh.write(png)
    return mean, rms, p95


def tongue_stats(curve):
    """Count and mean peak-to-valley depth against a wide moving baseline."""
    n = len(curve)
    half = max(n // 50, 2)
    base = [sum(curve[(i + k) % n] for k in range(-half, half + 1)) / (2 * half + 1)
            for i in range(n)]
    resid = [curve[i] - base[i] for i in range(n)]
    peaks = [i for i in range(n)
             if resid[i] > 0.015
             and resid[i] >= resid[i - 1] and resid[i] >= resid[(i + 1) % n]]
    merged = []
    for p in peaks:
        if merged and (p - merged[-1]) % n < max(n // 200, 2):
            continue
        merged.append(p)
    valleys = [i for i in range(n)
               if resid[i] < -0.015
               and resid[i] <= resid[i - 1] and resid[i] <= resid[(i + 1) % n]]
    depths = []
    span = max(n // 25, 4)
    for p in merged:
        near = [abs(resid[v]) for v in valleys
                if (v - p) % n < span or (p - v) % n < span]
        if near:
            depths.append(resid[p] + max(near))
    return len(merged), (sum(depths) / len(depths) if depths else 0.0)


def write_plot(path, ref, gen):
    W, H = 1024, 300
    n = len(ref)
    top = max(max(ref), max(gen)) * 1.05
    img = [[30] * W for _ in range(H)]
    for i in range(W):
        f = i / W * n
        i0 = int(f) % n
        fr = f - int(f)
        rv = ref[i0] * (1 - fr) + ref[(i0 + 1) % n] * fr
        y = H - 1 - int(rv / top * (H - 10))
        for yy in range(max(0, y), H):
            img[yy][i] = 90
    for i in range(W):
        f = i / W * n
        i0 = int(f) % n
        fr = f - int(f)
        gv = gen[i0] * (1 - fr) + gen[(i0 + 1) % n] * fr
        y = H - 1 - int(gv / top * (H - 10))
        for yy in range(max(0, y - 1), min(H, y + 2)):
            img[yy][i] = 255
    raw = b""
    for row in img:
        raw += b"\x00" + bytes(row)

    def chunk(t, d):
        c = t + d
        return struct.pack(">I", len(d)) + c + struct.pack(">I", zlib.crc32(c) & 0xffffffff)

    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 0, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 9))
           + chunk(b"IEND", b""))
    with open(path, "wb") as fh:
        fh.write(png)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(os.path.dirname(here))
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--reference", default=os.path.join(here, "aura_reference.png"))
    ap.add_argument("--mask", default=None,
                    help="measure this mask instead of rendering one")
    ap.add_argument("--keep-mask", default=None,
                    help="also save the rendered mask here")
    ap.add_argument("--plot", default=None)
    args = ap.parse_args()

    if args.mask:
        mask = args.mask
    else:
        mask = args.keep_mask or os.path.join(tempfile.gettempdir(),
                                              "aura-check-mask.png")
        render_mask(root, mask)

    ref = boundary(args.reference)
    gen = boundary(mask)

    n = len(ref)
    diffs = [gen[i] - ref[i] for i in range(n)]
    rms = math.sqrt(sum(d * d for d in diffs) / n)
    bias = sum(diffs) / n
    worst = min(diffs)

    rc, rd = tongue_stats(ref)
    gc, gd = tongue_stats(gen)

    fm, fr, fp95 = field_stats(args.reference, mask,
                               diff_png=(args.plot + ".diff.png") if args.plot else None)
    print("field     : mean|d| %.4f  rms %.4f  p95 %.4f  (full scale = 1)"
          % (fm, fr, fp95))

    # Width-per-height profiles need no anchor at all: for every height band
    # of the thresholded silhouette, the left and right extents, normalised
    # to the shape's own bounding box. Radial-from-anchor comparisons let
    # the anchor drift with the mass they measure - shrink the base and the
    # anchor follows it, so the numbers move against the geometry. Widths
    # against the box cannot do that.
    rw = width_profile(args.reference)
    gw = width_profile(mask)
    wd = [gw[i][0] - rw[i][0] for i in range(len(rw))]        + [gw[i][1] - rw[i][1] for i in range(len(rw))]
    wrms = math.sqrt(sum(d * d for d in wd) / len(wd))
    print("width rms : %.4f  (left+right half-widths, box units)" % wrms)
    for name, band in (("base", (0.02, 0.18)), ("waist", (0.30, 0.50)),
                       ("crown", (0.80, 0.98))):
        lo, hi = band
        idx = range(int(lo * len(rw)), int(hi * len(rw)))
        rr = sum(rw[i][0] + rw[i][1] for i in idx) / len(idx)
        gg = sum(gw[i][0] + gw[i][1] for i in idx) / len(idx)
        print("  %-5s width: ref %.3f  gen %.3f  (%+.3f)"
              % (name, rr, gg, gg - rr))

    print("reference : %2d tongues, mean depth %.3f" % (rc, rd))
    print("pipeline  : %2d tongues, mean depth %.3f" % (gc, gd))
    print("deviation : rms %.4f  bias %+.4f  worst %+.4f  (crown radii)"
          % (rms, bias, worst))
    if args.plot:
        write_plot(args.plot, ref, gen)
        print("plot      : %s" % args.plot)


if __name__ == "__main__":
    main()
