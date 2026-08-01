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

The strip is built as three bands along V, matching how the source material
layers a ki aura - a bright core hugging the body, a dense flame above it, and
sparse discharge at the outside:

    0.00 .. rim      inner shoulder: alpha climbs from nothing to the hottest point
    rim  .. solid    dense body, broken by striations running along V
    solid .. 1.00    flame licks in two tiers, plus detached embers

Two details matter more than they look. The envelope *falls* toward the tips
rather than holding full alpha: the stage blends additively, so a strip that
stays opaque to its outer row clips to flat white and the aura loses every
internal edge - which is exactly what a single solid band produced. And the
licks come in two tiers, a few long ones over many short ones, because a comb
of equal spikes reads as a sawblade no matter how much per-spike jitter is
applied to it.

The body band is kept deliberately thin and the taper steep, so most of the
strip's area is gap rather than flame. Reference art draws a ki aura as
separate needles with the scene plainly visible between them; a wide body and
a shallow taper give broad scallops that meet at the base, and once the strip
is wrapped several times around the ring those merge into one lit shell that
hides both the character and the world behind it.
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


# How far either side of its axis a lick's profile reaches before falling back
# to the body, in cell widths. Past 0.5 so neighbouring feet overlap and the
# flame has no gaps at its base; the needles themselves are far narrower than
# this, because the profile exponent takes the height down long before the
# foot runs out. See flame_boundary.
LICK_HALF = 0.9


def hash01(index, salt):
    """Deterministic 0..1 from an integer. Reproducible and, because it reads
    only the index, still exactly periodic once the caller wraps that index."""
    x = math.sin((index + 1) * 12.9898 + salt * 78.233) * 43758.5453
    return x - math.floor(x)


def envelope(v, rim, solid, tip_alpha):
    """Brightness through the aura's depth, before any silhouette is cut.

    Three things are shaped here. The innermost row is fully transparent, so
    the aura emerges from behind the player instead of starting somewhere -
    the ring's inner edge is a closed loop of geometry, and any alpha at all
    on that row draws the loop itself, which is a hard oval outline sitting
    over the character. That outline was the single most artificial thing
    about the effect and no amount of colour work hides it. The hottest point
    is just outside the fade, which is where the source material puts the
    core. And everything past it falls away, so the tips are thin energy
    instead of the white shards a flat alpha produces.
    """
    if v < rim:
        # Smoothstep rather than linear: a linear ramp meeting the plateau
        # creases, and the crease reads as a second, unwanted rim.
        return smoothstep(0.0, rim, v)

    t = (v - rim) / max(1.0 - rim, 1e-6)
    return 1.0 + (tip_alpha - 1.0) * t * t


def striations(u, v, spikes, solid, depth):
    """Grooves running along V, dividing the body into flame streaks.

    Without these the body is a single flat value over most of the aura's
    area, and an additive blend turns a flat value into a featureless slab.

    One groove per lick, placed on the cell boundary so each streak runs up
    into the lick above it. Running them at their own frequency instead - the
    obvious thing, and what a first pass did - crosses the lick pattern and
    the two beat against each other into something that reads as corrugated
    metal rather than as flame.
    """
    # Fade in away from the inner rim: grooves reaching the innermost rows
    # would break up the core, which wants to stay continuous.
    weight = smoothstep(0.0, solid * 0.9, v)

    # 1 at the cell centre, 0 on the boundary.
    ridge = 0.5 + 0.5 * math.cos(2.0 * math.pi * u * spikes)

    # Raised so the darkening is confined to a narrow groove either side of the
    # boundary and most of the streak stays at full value.
    coarse = depth * pow(1.0 - ridge, 2.0)

    # A second set several times finer, running the whole depth. The reference
    # shows each lick carrying its own lengthwise grain, not just a division
    # between one lick and the next - it is what makes the flame read as having
    # been brushed on rather than filled.
    fine = 0.5 + 0.5 * math.cos(2.0 * math.pi * u * spikes * 3.0 + 1.7)
    coarse += depth * 0.55 * pow(1.0 - fine, 1.5)

    return 1.0 - weight * min(coarse, 0.85)


def filaments(u, spikes, fine, taper):
    """Fine needles riding on the outer edge of the licks.

    The reference's licks do not end in a point - they come apart into hairs,
    several per lick, at a much finer scale than the lick itself. Built from
    the same profile at a multiple of the frequency, so the two agree about
    what a needle looks like and the fray reads as the lick splitting rather
    than as separate detail laid over it.
    """
    count = max(int(round(spikes * fine)), 1)
    scaled = u * count
    index = int(math.floor(scaled))

    best = 0.0
    for offset in (-1, 0, 1):
        cell = index + offset
        wrapped = cell % count
        frac = scaled - cell

        d = abs(frac - 0.5) / LICK_HALF
        if d >= 1.0:
            continue

        height = 1.0 - 0.65 * hash01(wrapped, 11.0)
        best = max(best, height * pow(1.0 - d, taper))

    return best


def flame_boundary(u, spikes, jitter, solid, taper, minor, fray, fine):
    """How far out the flame reaches at this u, in V. Periodic in u.

    The silhouette is a height field rather than a set of drawn shapes, and
    that is what lets the licks be needles.

    Drawing each lick as a tapering quad - the obvious construction, and what
    this did first - ties how narrow a lick can be to how far apart they are.
    Anything narrower than its cell leaves the body's flat top showing between
    licks as a hard horizontal line, so the licks have to stay wide enough to
    overlap at the base, and wide licks meeting at the base are scallops. The
    reference art is the opposite: thin needles with real gaps between them,
    and the gaps read as flame every bit as much as the needles do.

    Expressed as a boundary the constraint disappears. Each lick contributes a
    profile that falls from its own height back down to `solid`, the gaps fall
    to `solid` on their own, and the body below is continuous whatever the
    licks do. A lick can then be as thin as the texel grid allows.

    Licks come in two tiers. A comb of same-height spikes reads as machined no
    matter how much the heights are jittered, because the eye picks up the
    constant period before it picks up the variation; giving one lick in three
    a full-length reach breaks that period at a scale the eye reads as flame.
    """
    scaled = u * spikes
    index = int(math.floor(scaled))

    boundary = solid

    # A leaning lick crosses into its neighbours' cells, so its own cell is not
    # enough. Three cells is sufficient because both the lean and the profile's
    # reach are bounded inside one cell width.
    for offset in (-1, 0, 1):
        cell = index + offset
        wrapped = cell % spikes            # keeps the u = 1 -> 0 seam exact
        frac = scaled - cell

        major = (wrapped * 3) % 7 < 3
        reach = 1.0 if major else minor

        height = reach * (1.0 - jitter * hash01(wrapped, 1.0))
        lean = (hash01(wrapped, 2.0) - 0.5) * 0.6 * jitter

        # Horizontal distance from this lick's axis, 0 at the axis and 1 where
        # its profile has fallen back to the body.
        d = abs(frac - (0.5 + lean)) / LICK_HALF
        if d >= 1.0 or height <= 0.0:
            continue

        # An exponent above 1 drops the profile away fast either side of the
        # axis, so the lick is a spine with a flared foot rather than a
        # triangle. This is the needle shape; at 1 it is a plain cone.
        boundary = max(boundary,
                       solid + (1.0 - solid) * height * pow(1.0 - d, taper))

    # Scaled by how far the lick already reaches, so the fray is a property of
    # the tip. Applied flat it would chew the body's outer edge into fur.
    boundary += fray * (boundary - solid) * filaments(u, spikes, fine, taper)

    return boundary


def embers(u, v, spikes, solid, softness):
    """Detached specks past the lick tips - the aura's outer discharge layer.

    Small and sparse on purpose: they exist to break the silhouette's outer
    line so it does not read as one cut edge, and anything larger stops
    looking like it came off the flame and starts looking like a second ring.
    """
    scaled = u * spikes
    index = int(math.floor(scaled))

    best = 0.0
    for offset in (-1, 0, 1):
        cell = index + offset
        wrapped = cell % spikes

        # Roughly one cell in six carries an ember, chosen off a different
        # salt than the licks so they do not all sit over the long ones.
        if hash01(wrapped, 5.0) > 0.18:
            continue

        cu = cell + hash01(wrapped, 6.0)
        cv = solid + (1.0 - solid) * (0.55 + 0.42 * hash01(wrapped, 7.0))
        radius = 0.07 + 0.05 * hash01(wrapped, 8.0)

        # Deliberately taller than wide: the strip wraps the ring several times
        # over, so U is the stretched axis on screen and a speck authored round
        # here comes out as a horizontal dash. Elongating it along V instead
        # lands it somewhere near round, and errs toward a rising spark.
        du = (scaled - cu) / max(radius, 1e-6)
        dv = (v - cv) / max(radius * (1.0 - solid), 1e-6)
        dist = math.sqrt(du * du + dv * dv)

        best = max(best, 1.0 - smoothstep(1.0 - softness * 4.0, 1.0, dist))

    return best * 0.45


def spike_alpha(u, v, spikes, jitter, solid, softness, taper, minor,
                striation, tip_alpha, rim, fray, fine):
    """Alpha at (u, v), both 0..1. Periodic in u so the strip tiles."""
    boundary = flame_boundary(u, spikes, jitter, solid, taper, minor,
                              fray, fine)

    # Feathered in V. The boundary is a height, so the edge softens along the
    # direction the flame grows - which is what keeps a near-vertical needle
    # flank from stair-stepping without also blurring the tip into a smudge.
    coverage = 1.0 - smoothstep(boundary - softness, boundary, v)

    flame = (coverage
             * envelope(v, rim, solid, tip_alpha)
             * striations(u, v, spikes, solid, striation))

    return max(flame, embers(u, v, spikes, solid, softness))


def build_png(width, height, spikes, jitter, peak, solid, softness, taper,
              minor, striation, tip_alpha, rim, fray, fine):
    rows = []
    for y in range(height):
        v = (y + 0.5) / height
        row = bytearray()
        row.append(0)                      # filter type 0 (None)
        for x in range(width):
            u = (x + 0.5) / width
            a = spike_alpha(u, v, spikes, jitter, solid, softness, taper,
                            minor, striation, tip_alpha, rim, fray, fine)
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
    ap.add_argument("--width", type=int, default=512)
    ap.add_argument("--height", type=int, default=256)
    ap.add_argument("--spikes", type=int, default=8,
                    help="licks across one tile (default: 8)")
    ap.add_argument("--jitter", type=float, default=0.45,
                    help="0 = every lick identical, 1 = highly varied")
    ap.add_argument("--peak", type=int, default=240,
                    help="brightest alpha value (default: 240)")
    ap.add_argument("--solid", type=float, default=0.14,
                    help="fraction of V that is unbroken body (default: 0.14)")
    ap.add_argument("--softness", type=float, default=0.07,
                    help="lick edge feather in V, 0 = aliased hard edge")
    ap.add_argument("--taper", type=float, default=7.5,
                    help="lick profile exponent; 1 = straight triangle, higher = "
                         "needle with concave flanks (default: 7.5)")
    ap.add_argument("--minor", type=float, default=0.22,
                    help="reach of a short lick against a long one (default: 0.22)")
    ap.add_argument("--striation", type=float, default=0.28,
                    help="depth of the streaks through the body, 0 = flat slab")
    ap.add_argument("--tip-alpha", type=float, default=0.52,
                    help="alpha at the outer row against the core (default: 0.52)")
    ap.add_argument("--fray", type=float, default=0.35,
                    help="how far the lick tips come apart into filaments")
    ap.add_argument("--fine", type=float, default=4.0,
                    help="filaments per lick (default: 4)")
    ap.add_argument("--rim", type=float, default=0.12,
                    help="fraction of V the inner shoulder fades across")
    args = ap.parse_args()

    if args.spikes < 1:
        sys.exit("--spikes must be at least 1")
    if args.width % args.spikes:
        print("warning: width %d is not a multiple of spikes %d; the tile seam "
              "may show" % (args.width, args.spikes), file=sys.stderr)

    blob = build_png(args.width, args.height, args.spikes, args.jitter,
                     args.peak, args.solid, args.softness, args.taper,
                     args.minor, args.striation, args.tip_alpha, args.rim,
                     args.fray, args.fine)
    with open(args.output, "wb") as fh:
        fh.write(blob)

    print("%s: %dx%d, %d spikes, %d bytes"
          % (args.output, args.width, args.height, args.spikes, len(blob)))


if __name__ == "__main__":
    main()
