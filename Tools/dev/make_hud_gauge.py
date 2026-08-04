#!/usr/bin/env python3
"""Generate the HUD panel: its plate and its gauge capsules.

The old panel put plCurrent, plFatigue, plHealth and plMaximum into one bar as
seven overlapping coloured regions. Four of those sat in the same red-orange
range, which is the discrimination the common colour vision deficiencies lose,
and no amount of recolouring fixes a bar carrying four nested quantities. The
panel this generates gives each value its own gauge instead, so the power bar
becomes a single fill and nothing has to be layered at all.

`interface/hud/main.png` cannot be reused for that. It is one 800x200 texture
with the band, the portrait surround and a single capsule baked together, and
its capsule ends at virtual y+58.7 with the HUD box bottoming out on the
480-line - there is no room in it for a second gauge, let alone three. So the
panel is generated, and main.png is simply no longer drawn.

Everything here is in the material main.png established, taken by sampling it
down its centre column, so the new panel is the same object rather than an
imitation of it:

    glow    4px   (47,101,157) fading out, the halo that lifts it off the scene
    border  8px   (45,87,131) to (24,64,109) on top, inverted underneath
    glass  24px   light over the top half, breaking to dark at 53%

The gauges keep the HUD's own draw order - a plain whiteShader fill underneath,
the frame over it - which is where the bars get their gloss.

Described rather than drawn, for the same reason as the aura mesh: a generator
is easier to re-aim than a binary, and the numbers stay visible.

Authored at 4x the virtual size so the caps stay round once the 640x480 layout
is scaled up to the display, and supersampled 3x3 because a cap radius shows
every stair step. The plate is drawn at half that: it is mostly flat, and only
its corners carry curvature worth resolving.

usage:
    make_hud_gauge.py <output-directory>
"""
import argparse
import os
import struct
import sys
import zlib


# The panel's geometry, in the 640x480 virtual space the HUD is authored in.
# These mirror the HUD_* defines in Game/CGame/cg_local.h: the art has to be
# drawn at the size the layout reserves for it, or the frames land off their
# windows. Change them together.
HUD_PANEL_WIDTH = 288
HUD_PANEL_HEIGHT = 86
HUD_PORTRAIT = 70
HUD_BAR_WIDTH = 76
HUD_ROW_PRIMARY = 12
HUD_ROW_SECONDARY = 8
HUD_ROW_MINOR = 6


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


# Interior gloss down the capsule, as (position, r, g, b, a) read off main.png.
# The break at 0.53 is a hard one in the original and stays hard here: it is the
# horizon line that makes the strip read as curved glass rather than as a tint.
GLASS = (
    (0.00, 198, 209, 209, 149),
    (0.15, 196, 198, 198, 116),
    (0.50, 170, 170, 170, 113),
    (0.52, 153, 153, 153,  98),
    (0.53,  58,  58,  58,  51),
    (0.80,  37,  37,  37,  51),
    (0.88,  56,  63,  63,  61),
    (1.00, 109, 132, 132, 110),
)


def _lerp(a, b, t):
    return a + (b - a) * t


def _ramp(stops, t):
    """Sample a table of (position, r, g, b, a) stops."""
    if t <= stops[0][0]:
        return stops[0][1:]
    for lo, hi in zip(stops, stops[1:]):
        if t <= hi[0]:
            span = hi[0] - lo[0]
            f = 0.0 if span <= 0 else (t - lo[0]) / span
            return tuple(_lerp(lo[i], hi[i], f) for i in range(1, 5))
    return stops[-1][1:]


def capsule(width, height, glow, border, samples=3):
    """A stadium frame: glass across the middle, border ring, glow outside.

    Concentric because both radii are measured from the same core segment, so
    the ring keeps its thickness around the caps instead of pinching.
    """
    inner = height / 2.0 - glow - border          # interior half-height
    core_y = height / 2.0
    core_x0 = glow + border + inner
    core_x1 = width - glow - border - inner

    def distance(px, py):
        dx = max(core_x0 - px, 0.0, px - core_x1)
        dy = py - core_y
        return (dx * dx + dy * dy) ** 0.5

    def sample(px, py):
        d = distance(px, py)
        if d < inner:
            t = (py - (core_y - inner)) / (2.0 * inner)
            r, g, b, a = _ramp(GLASS, min(max(t, 0.0), 1.0))
            return (r, g, b, a)
        if d < inner + border:
            # Dark rim on top, lighter underneath: the capsule is lit from above
            # and the underside is catching the band it sits on.
            f = (d - inner) / border
            if py < core_y:
                return (_lerp(24, 45, f), _lerp(64, 87, f), _lerp(109, 131, f), 255)
            return (_lerp(22, 46, f), _lerp(72, 100, f), _lerp(120, 146, f), 255)
        if d < inner + border + glow:
            f = (d - inner - border) / glow
            return (_lerp(58, 28, f), _lerp(111, 84, f), _lerp(162, 148, f),
                    _lerp(163, 0, f))
        return (0.0, 0.0, 0.0, 0.0)

    step = 1.0 / samples

    def pixel(x, y):
        # Averaged premultiplied, so the caps fade to transparent instead of
        # dragging the border's blue out into a halo.
        pr = pg = pb = pa = 0.0
        for sy in range(samples):
            for sx in range(samples):
                r, g, b, a = sample(x + (sx + 0.5) * step, y + (sy + 0.5) * step)
                w = a / 255.0
                pr += r * w
                pg += g * w
                pb += b * w
                pa += a
        n = samples * samples
        pa /= n
        if pa <= 0.5:
            return (0, 0, 0, 0)
        scale = n * (pa / 255.0)
        return (int(round(pr / scale)), int(round(pg / scale)),
                int(round(pb / scale)), int(round(pa)))

    return pixel


def rounded(width, height, radius, border, glow, fill=None, gradient=0,
            samples=3):
    """A rounded rectangle in the capsule's material.

    `fill` None leaves the middle transparent; a colour fills it instead, which
    is what the plate enclosing the panel wants.
    """
    x0, y0 = glow + 0.0, glow + 0.0
    x1, y1 = width - glow, height - glow
    r = max(radius, border)

    def distance(px, py):
        dx = max(x0 + r - px, 0.0, px - (x1 - r))
        dy = max(y0 + r - py, 0.0, py - (y1 - r))
        return (dx * dx + dy * dy) ** 0.5 - r

    def sample(px, py):
        d = distance(px, py)
        if d < -border:
            if fill is None:
                return (0.0, 0.0, 0.0, 0.0)
            # Lit from above, like every other surface on this HUD.
            t = 0.5 - (py - y0) / max(1.0, y1 - y0)
            return tuple(min(255.0, max(0.0, fill[i] + gradient * t * 2))
                         for i in range(3)) + (fill[3],)
        if d < 0:
            f = (d + border) / border
            return (_lerp(46, 28, f), _lerp(100, 74, f), _lerp(146, 118, f), 255)
        if d < glow:
            f = d / glow
            return (47.0, 101.0, 157.0, _lerp(150, 0, f))
        return (0.0, 0.0, 0.0, 0.0)

    step = 1.0 / samples

    def pixel(x, y):
        pr = pg = pb = pa = 0.0
        for sy in range(samples):
            for sx in range(samples):
                cr, cg, cb, ca = sample(x + (sx + 0.5) * step,
                                        y + (sy + 0.5) * step)
                w = ca / 255.0
                pr += cr * w
                pg += cg * w
                pb += cb * w
                pa += ca
        n = samples * samples
        pa /= n
        if pa <= 0.5:
            return (0, 0, 0, 0)
        scale = n * (pa / 255.0)
        return (int(round(pr / scale)), int(round(pg / scale)),
                int(round(pb / scale)), int(round(pa)))

    return pixel


SHEAR = 0.36397023426620234                 # tan(20 degrees), the deck's cut


def deny_mark(size=64, thickness=0.13, inset=0.10, soft=0.035):
    """The skill bar's refusal mark: a sheared frame with a bar struck through.

    Drawn white and tinted by trap_R_SetColor, the way the radar marks and every
    other bare .png in this interface are - the shader the engine builds for a
    loose image is rgbGen vertex, so the colour belongs to the caller and this
    only decides the shape. cg_weapons.c draws it twice over: small and steady
    in the guard colour for a skill whose requirements are unmet, large and
    decaying in the threat colour for a change just refused.

    The frame is a parallelogram cut at the deck's 20 degrees rather than a
    circle, because it sits on a skill icon that is a plain square and has to
    read as interface rather than as part of the art. The strike is the
    anti-diagonal: it crosses both sheared edges instead of running near-parallel
    to either, which is what keeps the mark legible at the eleven virtual units
    the corner badge is drawn at.
    """
    samples = 4

    def alpha(u, v):
        # The frame leans the way the panel edges do: top edge pushed right.
        lean = SHEAR * (0.5 - v) * 0.5
        left, right = inset + lean, 1.0 - inset + lean
        top, bottom = inset, 1.0 - inset
        # Distance inside the parallelogram, negative outside it.
        d = min(u - left, right - u, v - top, bottom - v)
        frame = min(d / soft, (thickness - d) / soft, 1.0)
        # The strike, clipped to the frame's own footprint so it does not
        # overhang the badge.
        strike = min((thickness * 0.75 - abs(u - v) * 0.7071) / soft,
                     (d + thickness) / soft, 1.0)
        return max(min(max(frame, strike), 1.0), 0.0)

    def pixel(x, y):
        total = 0.0
        for sy in range(samples):
            for sx in range(samples):
                u = (x + (sx + 0.5) / samples) / size
                v = (y + (sy + 0.5) / samples) / size
                total += alpha(u, v)
        a = int(round(255.0 * total / (samples * samples)))
        return (255, 255, 255, max(0, min(255, a)))

    return pixel


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("outdir")
    ap.add_argument("--scale", type=int, default=4,
                    help="texture pixels per virtual pixel (default: 4)")
    ap.add_argument("--length", type=int, default=HUD_BAR_WIDTH,
                    help="width of every gauge window, in virtual pixels")
    ap.add_argument("--border", type=int, default=2,
                    help="border thickness in virtual pixels (default: 2)")
    ap.add_argument("--glow", type=int, default=1,
                    help="glow width in virtual pixels (default: 1)")
    args = ap.parse_args()

    for name in ("scale", "length", "border", "glow"):
        if getattr(args, name) < 1:
            sys.exit("--%s must be at least 1" % name)

    s, pad = args.scale, 2 * args.border + 2 * args.glow
    os.makedirs(args.outdir, exist_ok=True)
    written = []

    def emit(name, w, h, pixel):
        size = write_png(os.path.join(args.outdir, name + ".png"), w, h, pixel)
        written.append("%s %dx%d %db" % (name, w, h, size))

    # One capsule per rank. The window width never changes, so the four gauges
    # stay directly comparable and only their height says which outranks which.
    for name, interior in (("gaugePrimary", HUD_ROW_PRIMARY),
                           ("gaugeSecondary", HUD_ROW_SECONDARY),
                           ("gaugeMinor", HUD_ROW_MINOR)):
        w, h = (args.length + pad) * s, (interior + pad) * s
        emit(name, w, h, capsule(w, h, args.glow * s, args.border * s))

    # The plate that encloses the panel. Half scale: it is mostly flat, and only
    # its corners carry any curvature worth resolving. No portrait frame - the
    # character icons ship with one of their own, and a second around it reads
    # as a mistake.
    ps = max(1, s // 2)
    emit("hudPlate", HUD_PANEL_WIDTH * ps, HUD_PANEL_HEIGHT * ps,
         rounded(HUD_PANEL_WIDTH * ps, HUD_PANEL_HEIGHT * ps, 10 * ps,
                 args.border * ps, args.glow * ps, fill=(15, 27, 40, 236),
                 gradient=14))

    # The skill bar's refusal mark. Square and generously oversized: it is drawn
    # at anything from eleven to twenty-eight virtual units and the strike has to
    # survive the smaller of those.
    emit("skillDeny", 64, 64, deny_mark(64))

    print("wrote %s into %s" % (", ".join(written), args.outdir))


if __name__ == "__main__":
    main()
