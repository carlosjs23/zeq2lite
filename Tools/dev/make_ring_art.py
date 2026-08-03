#!/usr/bin/env python3
"""Generate the tournament ring's floor and boundary art.

Five treatments of the same problem: the ring in rules/arena_desert.def is a
1024-unit circle of open desert with nothing to see, and a fighter has to know
where its edge is while looking at his opponent rather than at the floor.

    R1 hairline   one additive line on the ground and nothing else
    R2 flagstone  laid stone floor with a braided rope kerb
    R3 kiwall     an additive wall that lights as a fighter nears it
    R4 budokai    square tiles, a painted boundary and corner posts
    R5 pillars    eight energy markers standing on a faint ground ring

Each writes the textures it needs plus its shader blocks. R4 and R3 are the
combination that ships: cgame builds the floor, the corner posts and the ki
wall out of scene polys in cg_arena.c, from the radius in the per-map arena
file. That is why those blocks carry no $lightmap stage and take their colour
from the vertices - a scene poly has no lightmap coordinates, and the wall's
brightness is the local fighter's distance to the boundary, which only the
client knows. R1, R2 and R5 are unwired candidates and are kept as authored.

Deliberately stdlib-only, like every other generator in this directory - the
noise, the tiling and the PNG writing are all hand-rolled so a build machine
needs nothing installed.

usage:
    make_ring_art.py <mod-directory>
"""
import argparse
import math
import os
import struct
import sys
import zlib


# The ring, straight out of rules/arena_desert.def. Only the radius matters to
# the art: it sets how many times a kerb tile repeats around the circumference,
# and getting that wrong is what makes a rope look like a chain.
RING_RADIUS = 1024.0

# The deck's gold, shared with the training UI so the ring and the HUD agree.
KI = (0xE8, 0xA3, 0x3D)
KI_HOT = (0xFF, 0xE3, 0xA8)
GAUGE = (0x4F, 0xA3, 0xE3)

# Desert stone, sampled off the map's own cliff texture so a laid floor sits in
# the same family as the rock it is laid on rather than looking imported.
STONE_LIGHT = (0xC6, 0xB2, 0x93)
STONE_MID = (0xA8, 0x93, 0x73)
STONE_DARK = (0x6E, 0x5F, 0x4A)
GROUT = (0x4A, 0x40, 0x33)
TILE_TAN = (0xD8, 0xC4, 0x9E)


# ------------------------------------------------------------------ plumbing

def write_png(path, w, h, px):
    rows = bytearray()
    stride = w * 4
    for y in range(h):
        rows.append(0)
        rows += px[y * stride:(y + 1) * stride]

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    blob = (b"\x89PNG\r\n\x1a\n" +
            chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(bytes(rows), 9)) +
            chunk(b"IEND", b""))
    with open(path, "wb") as fh:
        fh.write(blob)
    return len(blob)


def blank(w, h, rgba=(0, 0, 0, 0)):
    return bytearray(bytes(rgba) * (w * h))


def put(px, w, x, y, rgb, a=255):
    i = (y * w + x) * 4
    px[i] = rgb[0]
    px[i + 1] = rgb[1]
    px[i + 2] = rgb[2]
    px[i + 3] = a


def over(px, w, x, y, rgb, a):
    """Source-over, so a pattern can be laid on a floor already written."""
    if a <= 0:
        return
    i = (y * w + x) * 4
    f = a / 255.0
    for k in range(3):
        px[i + k] = int(px[i + k] * (1.0 - f) + rgb[k] * f)
    px[i + 3] = max(px[i + 3], a)


def lerp(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))


def smooth(t):
    return t * t * (3.0 - 2.0 * t)


# ------------------------------------------------------------------- noise

def _hash(x, y, seed):
    n = (x * 374761393 + y * 668265263 + seed * 1442695040888963407) & 0xFFFFFFFF
    n = (n ^ (n >> 13)) * 1274126177 & 0xFFFFFFFF
    return ((n ^ (n >> 16)) & 0xFFFF) / 65535.0


def value_noise(x, y, period, seed):
    """Lattice value noise that wraps at `period`, so the sheet tiles."""
    xi, yi = int(math.floor(x)), int(math.floor(y))
    xf, yf = x - xi, y - yi
    u, v = smooth(xf), smooth(yf)
    out = 0.0
    for dy in (0, 1):
        for dx in (0, 1):
            h = _hash((xi + dx) % period, (yi + dy) % period, seed)
            wx = u if dx else 1.0 - u
            wy = v if dy else 1.0 - v
            out += h * wx * wy
    return out


def fbm(x, y, period, seed, octaves=4):
    total, amp, norm, p = 0.0, 1.0, 0.0, period
    for o in range(octaves):
        total += value_noise(x * (1 << o), y * (1 << o), p * (1 << o), seed + o) * amp
        norm += amp
        amp *= 0.5
    return total / norm


# --------------------------------------------------------------- treatments

def r1_hairline(out):
    """A boundary strip, additive, and the tick that marks the cardinals.

    Everything is in the alpha: the shader adds it, so the colour is flat gold
    and the falloff is what stops it reading as a drawn line on the sand.
    """
    w, h = 16, 256
    px = blank(w, h)
    for y in range(h):
        # v runs across the strip; the line is a hot core with a wide skirt
        t = abs((y / (h - 1.0)) * 2.0 - 1.0)
        core = max(0.0, 1.0 - (t / 0.06) ** 2)
        skirt = max(0.0, 1.0 - t) ** 3 * 0.42
        a = min(1.0, core + skirt)
        rgb = lerp(KI, KI_HOT, core)
        for x in range(w):
            put(px, w, x, y, rgb, int(a * 255))
    write_png(os.path.join(out, "ringEdgeLine.png"), w, h, px)

    w = h = 64
    px = blank(w, h)
    for y in range(h):
        for x in range(w):
            fx = abs(x / (w - 1.0) * 2.0 - 1.0)
            fy = y / (h - 1.0)
            a = max(0.0, 1.0 - (fx / 0.22) ** 2) * max(0.0, 1.0 - fy) ** 1.5
            put(px, w, x, y, lerp(KI, KI_HOT, 1.0 - fy), int(a * 255))
    write_png(os.path.join(out, "ringEdgeTick.png"), w, h, px)

    return ["""ringEdgeLine
{
	cull none
	{
		map textures/ring/ringEdgeLine.png
		blendfunc GL_SRC_ALPHA GL_ONE
		rgbGen wave sin 0.72 0.10 0 0.25
		tcMod scroll 0.03 0
	}
}""", """ringEdgeTick
{
	cull none
	{
		map textures/ring/ringEdgeTick.png
		blendfunc GL_SRC_ALPHA GL_ONE
		rgbGen identity
	}
}"""]


def r2_flagstone(out):
    """Irregular laid stone, and a rope kerb sized to the ring's circumference."""
    n = 512
    px = blank(n, n, (0, 0, 0, 255))
    cells = 6                          # flagstones across the sheet
    step = n / float(cells)
    for y in range(n):
        for x in range(n):
            # jitter the lattice so the joints are not a grid
            gx, gy = x / step, y / step
            jx = (value_noise(gx * 1.7, gy * 1.7, cells * 2, 11) - 0.5) * 0.30
            jy = (value_noise(gx * 1.7 + 5, gy * 1.7 + 5, cells * 2, 12) - 0.5) * 0.30
            cx, cy = gx + jx, gy + jy
            fx, fy = cx - math.floor(cx), cy - math.floor(cy)
            edge = min(fx, 1.0 - fx, fy, 1.0 - fy)
            grain = fbm(x / 42.0, y / 42.0, 12, 21)
            base = lerp(STONE_DARK, STONE_LIGHT, 0.30 + 0.55 * grain)
            # per-stone tint so neighbouring slabs are not the same rock
            sid = _hash(int(math.floor(cx)) % cells, int(math.floor(cy)) % cells, 31)
            base = lerp(base, STONE_MID, 0.35 * sid)
            if edge < 0.045:
                base = lerp(GROUT, base, edge / 0.045)
            put(px, n, x, y, base, 255)
    write_png(os.path.join(out, "ringFloorStone.png"), n, n, px)

    # The kerb. One tile has to span an exact number of rope twists or the seam
    # shows once per lap; 128 repeats around 2*pi*1024 is a twist every 50 units.
    w, h = 128, 64
    px = blank(w, h, (0, 0, 0, 255))
    twists = 5.0                       # whole twists per tile, so the seam meets
    for y in range(h):
        for x in range(w):
            u = x / float(w)
            v = y / (h - 1.0)
            # Strands run diagonally: the phase advances along the rope and is
            # sheared across it, which is what makes a lay rather than stripes.
            p = (u * twists + v * 0.85) % 1.0
            # one strand lobe per period, round in section, with a dark groove
            # where two lobes meet - the groove is what reads as rope at all
            lobe = math.sin(p * math.pi)
            groove = max(0.0, 1.0 - abs(p - 0.5) / 0.5) ** 6
            across = 1.0 - abs(v * 2.0 - 1.0)          # the kerb's own roundness
            shade = 0.20 + 0.62 * (lobe ** 0.55) * (0.35 + 0.65 * across)
            shade += 0.18 * groove * across
            rope = lerp((0x4A, 0x39, 0x22), (0xD2, 0xB4, 0x7C), min(1.0, shade))
            # the twist gap: darken hard between strands
            if lobe < 0.30:
                rope = lerp((0x24, 0x1C, 0x11), rope, lobe / 0.30)
            if abs(v * 2.0 - 1.0) > 0.90:
                rope = lerp(rope, (0x1E, 0x18, 0x11), 0.75)
            put(px, w, x, y, rope, 255)
    write_png(os.path.join(out, "ringRope.png"), w, h, px)

    return ["""ringFloorStone
{
	{
		map textures/ring/ringFloorStone.png
		rgbGen identity
	}
	{
		map $lightmap
		blendFunc GL_DST_COLOR GL_ZERO
		rgbGen identity
	}
}""", """ringRope
{
	{
		map textures/ring/ringRope.png
		rgbGen identity
	}
	{
		map $lightmap
		blendFunc GL_DST_COLOR GL_ZERO
		rgbGen identity
	}
}"""]


def r3_kiwall(out):
    """An additive curtain: dense scales at the foot, gone by head height."""
    w, h = 256, 256
    px = blank(w, h)
    for y in range(h):
        v = y / (h - 1.0)
        # the wall fades out upward, so it never becomes a box the map is in
        fade = max(0.0, 1.0 - v) ** 1.8
        for x in range(w):
            u = x / (w - 1.0)
            # interlocking scales, offset every other row
            row = int(v * 16.0)
            ox = 0.5 if row % 2 else 0.0
            sx = (u * 16.0 + ox) % 1.0 - 0.5
            sy = (v * 16.0) % 1.0 - 0.5
            d = math.sqrt(sx * sx + sy * sy) * 2.0
            scale = max(0.0, 1.0 - abs(d - 0.78) / 0.20)
            drift = fbm(u * 4.0, v * 4.0 + 3.0, 8, 41)
            a = fade * (0.20 + 0.80 * scale) * (0.55 + 0.65 * drift)
            put(px, w, x, y, lerp(GAUGE, KI_HOT, scale * 0.55), int(min(1.0, a) * 255))
    write_png(os.path.join(out, "ringKiWall.png"), w, h, px)

    # The curtain's shape is in the alpha channel and its colour is flat, so the
    # blend has to weight by alpha - a bare GL_ONE GL_ONE draws the whole quad
    # as a solid sheet of gold. And rgbGen is vertex rather than a wave: cgame
    # builds this cylinder itself and writes the proximity brightness into the
    # vertex colours, which a shader-side wave would overwrite.
    return ["""ringKiWall
{
	cull none
	surfaceparm nonsolid
	surfaceparm trans
	{
		map textures/ring/ringKiWall.png
		blendfunc GL_SRC_ALPHA GL_ONE
		rgbGen vertex
		tcMod scroll 0.015 0.05
	}
	{
		map textures/ring/ringKiWall.png
		blendfunc GL_SRC_ALPHA GL_ONE
		rgbGen vertex
		tcMod scale 2 1
		tcMod scroll -0.04 0.09
	}
}"""]


def r4_budokai(out):
    """Square tiles with a painted boundary, plus the corner post."""
    n = 512
    px = blank(n, n, (0, 0, 0, 255))
    cells = 8
    step = n / float(cells)
    for y in range(n):
        for x in range(n):
            fx = (x % step) / step
            fy = (y % step) / step
            edge = min(fx, 1.0 - fx, fy, 1.0 - fy)
            wear = fbm(x / 30.0, y / 30.0, 17, 51)
            base = lerp(TILE_TAN, STONE_MID, 0.18 + 0.42 * wear)
            if edge < 0.035:
                base = lerp(GROUT, base, edge / 0.035)
            # a chipped corner every few tiles, so the grid is not machined
            tid = _hash(int(x // step), int(y // step), 61)
            if tid > 0.86 and edge < 0.14:
                base = lerp(base, STONE_DARK, (0.14 - edge) / 0.14 * 0.55)
            put(px, n, x, y, base, 255)
    write_png(os.path.join(out, "ringFloorTile.png"), n, n, px)

    w, h = 64, 256
    px = blank(w, h, (0, 0, 0, 255))
    for y in range(h):
        v = y / (h - 1.0)
        for x in range(w):
            u = x / (w - 1.0)
            round_ = 1.0 - abs(u * 2.0 - 1.0)
            shade = 0.35 + 0.65 * round_ ** 0.6
            col = lerp((0x3A, 0x2E, 0x22), (0xB4, 0x97, 0x63), shade)
            if v < 0.10 or (0.44 < v < 0.50):
                col = lerp(col, KI, 0.65)          # the painted bands
            if v > 0.93:
                col = lerp(col, STONE_DARK, 0.6)   # the footing
            put(px, w, x, y, col, 255)
    write_png(os.path.join(out, "ringPost.png"), w, h, px)

    # No $lightmap stage: cgame draws the floor and the posts as scene polys,
    # which carry no lightmap coordinates, so the map's light arrives as vertex
    # colour from CG_LightVerts instead. A $lightmap stage on a poly surface
    # samples whatever texture coordinates the first stage left behind, which
    # is the arena floor tinted by an unrelated corner of the map's lightmap.
    return ["""ringFloorTile
{
	{
		map textures/ring/ringFloorTile.png
		rgbGen vertex
	}
}""", """ringPost
{
	cull none
	{
		map textures/ring/ringPost.png
		rgbGen vertex
	}
}"""]


def r5_pillars(out):
    """A standing beam and the ground glow it sits in."""
    w, h = 64, 512
    px = blank(w, h)
    for y in range(h):
        v = y / (h - 1.0)
        # bright at the base, thinning and fading with height
        taper = 1.0 - v * 0.55
        fade = (1.0 - v) ** 1.35
        for x in range(w):
            u = x / (w - 1.0) * 2.0 - 1.0
            r = abs(u) / max(0.08, taper)
            core = max(0.0, 1.0 - (r / 0.30) ** 2)
            halo = max(0.0, 1.0 - r) ** 2.4
            flick = 0.75 + 0.25 * fbm(u * 2.0, v * 9.0, 16, 71)
            a = min(1.0, (core + halo * 0.5) * fade * flick)
            put(px, w, x, y, lerp(KI, KI_HOT, core), int(a * 255))
    write_png(os.path.join(out, "ringPillar.png"), w, h, px)

    n = 256
    px = blank(n, n)
    c = (n - 1) / 2.0
    for y in range(n):
        for x in range(n):
            d = math.hypot(x - c, y - c) / c
            a = max(0.0, 1.0 - d) ** 2.6 * 0.85
            put(px, n, x, y, lerp(KI, KI_HOT, max(0.0, 1.0 - d * 1.6)), int(a * 255))
    write_png(os.path.join(out, "ringGroundGlow.png"), n, n, px)

    return ["""ringPillar
{
	cull none
	surfaceparm nonsolid
	surfaceparm trans
	{
		map textures/ring/ringPillar.png
		blendfunc GL_SRC_ALPHA GL_ONE
		rgbGen wave sin 0.78 0.14 0 0.31
	}
}""", """ringGroundGlow
{
	cull none
	surfaceparm nonsolid
	{
		map textures/ring/ringGroundGlow.png
		blendfunc GL_SRC_ALPHA GL_ONE
		rgbGen wave sin 0.60 0.12 0 0.19
	}
}"""]


TREATMENTS = (
    ("R1", "hairline", r1_hairline),
    ("R2", "flagstone", r2_flagstone),
    ("R3", "kiwall", r3_kiwall),
    ("R4", "budokai", r4_budokai),
    ("R5", "pillars", r5_pillars),
)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("mod", help="mod directory to write into")
    args = ap.parse_args(argv[1:])

    tex = os.path.join(args.mod, "textures", "ring")
    os.makedirs(tex, exist_ok=True)
    scripts = os.path.join(args.mod, "scripts")
    os.makedirs(scripts, exist_ok=True)

    blocks = ["// generated by Tools/dev/make_ring_art.py - do not edit",
              "// tournament ring treatments; ring radius %d" % RING_RADIUS]
    for rid, name, fn in TREATMENTS:
        blocks.append("")
        blocks.append("// ---- %s %s" % (rid, name))
        for b in fn(tex):
            blocks.append(b)
        print("%s %-10s written" % (rid, name))

    path = os.path.join(scripts, "ringTournament.shader")
    with open(path, "wb") as fh:
        fh.write(("\r\n".join(blocks) + "\r\n").encode())
    print("ringTournament.shader: %d blocks" % sum(1 for b in blocks if b.startswith("ring")))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
