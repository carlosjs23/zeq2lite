#!/usr/bin/env python3
"""Turn any aura art into the pipeline's reference form: luminance on black.

The bake scripts read a silhouette from alpha when it varies, else from
luminance - which assumes art shot on black. Generated images routinely
violate both: the usual failure is a *painted* checkerboard (constant alpha
255, checker pattern in the pixels), and the pattern drifts in phase and
level across the image, so it cannot be modelled and subtracted - it has to
be segmented away.

Three cases, detected in order:

1. Real alpha (it varies): composite over black, done.
2. Dark corners: already luminance-on-black, passed through.
3. Painted checker: the checker is neutral while an aura's rim is saturated,
   so background = neutral pixels flood-connected to the image border. The
   flood cannot leak into a neutral-white core because the saturated rim
   encloses it. The feathered edge keeps a checker tint, so a local
   background estimate is unmixed out of it, weighted by saturation; and the
   checker ghosting *through* the translucent core is averaged away over the
   pattern's own period, confined to neutral interior pixels where the art
   is smooth glow anyway.

Needs numpy and scipy - the only Tools/dev script that does; segmenting a
five-megapixel image in pure stdlib python is minutes, not seconds.

usage:
    aura_reference_clean.py <in.png> <out.png> [--color] [--neutral 22] [--edge 6]

--color writes RGB-on-black instead of luminance-on-black, for references
whose own colours should reach the strip (the band bake carries them since
the colour-strip change; a greyscale reference behaves as before).
"""

import argparse
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from png_sheet import decode_png

try:
    import numpy as np
    from scipy import ndimage
except ImportError:
    sys.exit("aura_reference_clean.py needs numpy and scipy "
             "(python3 -m pip install numpy scipy)")


def write_png(path, img):
    """img: h x w greyscale or h x w x 3 rgb, uint8."""
    grey = img.ndim == 2
    h, w = img.shape[:2]
    raw = b""
    for y in range(h):
        raw += b"\x00" + img[y].tobytes()

    def chunk(t, d):
        c = t + d
        return struct.pack(">I", len(d)) + c + struct.pack(">I", zlib.crc32(c) & 0xffffffff)

    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 0 if grey else 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 9))
           + chunk(b"IEND", b""))
    with open(path, "wb") as fh:
        fh.write(png)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("infile")
    ap.add_argument("outfile")
    ap.add_argument("--neutral", type=float, default=22.0,
                    help="max channel spread for a pixel to count as "
                         "unsaturated background (default 22)")
    ap.add_argument("--color", action="store_true",
                    help="write RGB on black rather than luminance")
    ap.add_argument("--edge", type=float, default=6.0,
                    help="pixels from the background over which edge "
                         "confidence ramps in (default 6)")
    args = ap.parse_args()

    w, h, px = decode_png(args.infile)
    P = np.array(px, dtype=np.float64)
    O = P[:, :, :3]
    A = P[:, :, 3]
    lum = O.mean(axis=2)

    if A.max() - A.min() > 32:
        if args.color:
            out = O * (A / 255.0)[:, :, None]
        else:
            out = lum * (A / 255.0)
        write_png(args.outfile, np.clip(out, 0, 255).astype(np.uint8))
        print("%s: real alpha, composited over black" % args.outfile)
        return

    corners = [lum[2, 2], lum[2, -3], lum[-3, 2], lum[-3, -3]]
    if max(corners) < 32:
        write_png(args.outfile,
                  np.clip(O if args.color else lum, 0, 255).astype(np.uint8))
        print("%s: already on black, passed through" % args.outfile)
        return

    # Painted checker. Segment: neutral and border-connected is background.
    chroma = O.max(axis=2) - O.min(axis=2)
    neutral = chroma < args.neutral
    lab, _ = ndimage.label(neutral)
    border = np.zeros_like(neutral)
    border[0, :] = border[-1, :] = border[:, 0] = border[:, -1] = True
    bg_labels = np.unique(lab[border & neutral])
    bg = np.isin(lab, bg_labels[bg_labels > 0])
    if bg.mean() < 0.05:
        sys.exit("no checker background found; is this already clean art?")

    # Unmix the checker out of the feathered edge: how much background a
    # pixel still contains falls with saturation and with distance in.
    dist = ndimage.distance_transform_edt(~bg)
    sat = np.clip(chroma / 45.0, 0.0, 1.0)
    edge = np.clip(dist / args.edge, 0.0, 1.0)
    a = np.maximum(sat, edge)
    Blocal = ndimage.grey_dilation(np.where(bg, lum, 0.0), size=61)
    if args.color:
        out = np.where(bg[:, :, None], 0.0,
                       np.clip(O - (Blocal * (1.0 - a))[:, :, None], 0.0, 255.0))
        outlum = out.mean(axis=2)
    else:
        out = np.where(bg, 0.0, np.clip(lum - Blocal * (1.0 - a), 0.0, 255.0))
        outlum = out

    # The checker ghosts through translucent neutral cores; average it away
    # over roughly its own period, only there.
    core = (chroma < args.neutral + 4) & (outlum > 40)
    core = ndimage.binary_erosion(core, iterations=6)
    if core.any():
        if args.color:
            for k in range(3):
                out[:, :, k] = np.where(core,
                                        ndimage.uniform_filter(out[:, :, k], size=76),
                                        out[:, :, k])
        else:
            out = np.where(core, ndimage.uniform_filter(out, size=76), out)

    write_png(args.outfile, np.clip(out, 0, 255).astype(np.uint8))
    print("%s: painted checker removed (%.0f%% background)"
          % (args.outfile, 100 * bg.mean()))


if __name__ == "__main__":
    main()
