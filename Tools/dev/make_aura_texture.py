#!/usr/bin/env python3
"""Generate the tiling spike strip the screen-space aura scrolls around its ring.

The existing effects/aura/auraSpike.png cannot serve here. It is a single soft
blob centred in a 256x256 image that fades to nothing at every edge - fine for
the convex-hull aura, which stamps one quad per spike, but the screen-space
technique wraps *one* texture around the whole ring and scrolls it, so the
texture has to repeat seamlessly along U.

Layout, matching glsl/aura_vp.glsl:

    U  runs around the ring and must tile seamlessly
    V  runs inner (0) to outer (1), i.e. along each spike's length

Colour convention follows the engine's other aura art: RGB is white and the
shape lives entirely in alpha. The fragment program premultiplies by that alpha
before adding, so the alpha is what shapes the glow.
"""

import argparse
import math
import struct
import sys
import zlib


def smoothstep(edge0, edge1, x):
    if edge1 == edge0:
        return 0.0
    t = (x - edge0) / (edge1 - edge0)
    t = max(0.0, min(1.0, t))
    return t * t * (3.0 - 2.0 * t)


def spike_alpha(u, v, spikes, jitter, solid, softness):
    """Alpha at (u, v), both 0..1. Periodic in u so the strip tiles.

    The silhouette follows the technique author's mockup: an opaque body from
    the inner rim outward, ending in hard-edged triangular spikes of varying
    length. Deliberately not a soft radial gradient - that reads as fur rather
    than flame, because nothing in it holds a defined edge.
    """
    # Fully opaque body. This is most of the aura's area and what gives it
    # presence against the world.
    if v <= solid:
        return 1.0

    # Which spike are we nearest, and how far across it? Working in spike-local
    # coordinates keeps the period exact, so the seam at u=1 -> u=0 is perfect.
    scaled = u * spikes
    index = int(math.floor(scaled))
    frac = scaled - index

    # Deterministic per-spike variation. Derived from the index rather than a
    # random source so the strip still tiles and regenerating is reproducible.
    phase = (index * 0.6180339887) % 1.0
    phase2 = (index * 0.2451224518) % 1.0

    height = 1.0 - jitter * phase              # some spikes fall well short
    lean = (phase2 - 0.5) * 0.5 * jitter       # and lean off vertical

    # Position within the spike band, 0 at the body's edge, 1 at the tip.
    vv = (v - solid) / max(1.0 - solid, 1e-6)

    if height <= 0.0 or vv >= height:
        return 0.0

    t = vv / height                            # 0 at this spike's base, 1 at its tip

    # A straight taper to a point: linear, so the edge stays a clean diagonal
    # rather than bulging. Adjacent spikes meet at the body, leaving no gap.
    half_width = 0.5 * (1.0 - t)
    centre = 0.5 + lean * t
    dist = abs(frac - centre)

    if half_width <= 0.0:
        return 0.0

    # Hard edge, softened by a pixel or two so the diagonal does not stair-step.
    edge = softness * 0.5
    return 1.0 - smoothstep(max(half_width - edge, 0.0), half_width, dist)


def build_png(width, height, spikes, jitter, peak, solid, softness):
    rows = []
    for y in range(height):
        v = (y + 0.5) / height
        row = bytearray()
        row.append(0)                      # filter type 0 (None)
        for x in range(width):
            u = (x + 0.5) / width
            a = spike_alpha(u, v, spikes, jitter, solid, softness)
            a = int(round(max(0.0, min(1.0, a)) * peak))
            row += bytes((255, 255, 255, a))
        rows.append(bytes(row))
    raw = b"".join(rows)

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("output")
    ap.add_argument("--width", type=int, default=256)
    ap.add_argument("--height", type=int, default=128)
    ap.add_argument("--spikes", type=int, default=8,
                    help="spikes across one tile (default: 8)")
    ap.add_argument("--jitter", type=float, default=0.45,
                    help="0 = every spike identical, 1 = highly varied")
    ap.add_argument("--peak", type=int, default=230,
                    help="brightest alpha value (default: 230)")
    ap.add_argument("--solid", type=float, default=0.45,
                    help="fraction of V that is fully opaque body (default: 0.45)")
    ap.add_argument("--softness", type=float, default=0.06,
                    help="spike edge softness, 0 = aliased hard edge")
    args = ap.parse_args()

    if args.spikes < 1:
        sys.exit("--spikes must be at least 1")
    if args.width % args.spikes:
        print("warning: width %d is not a multiple of spikes %d; the tile seam "
              "may show" % (args.width, args.spikes), file=sys.stderr)

    blob = build_png(args.width, args.height, args.spikes, args.jitter,
                     args.peak, args.solid, args.softness)
    with open(args.output, "wb") as fh:
        fh.write(blob)

    print("%s: %dx%d, %d spikes, %d bytes"
          % (args.output, args.width, args.height, args.spikes, len(blob)))


if __name__ == "__main__":
    main()
