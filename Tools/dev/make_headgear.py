#!/usr/bin/env python3
"""Generate headgear meshes and merge them into the masters' head models.

Why this exists. The IP-safe bald donor heads in this tree are krillin's and
nappa's, and five masters have to be told apart at a glance. Paint cannot do it:
three masters wearing one donor mesh are one silhouette in three colours, which
is exactly what the playtest reported - naida, seppa and rhogan all read as
Krillin. The silhouette is the thing that carries at distance, so the silhouette
is what has to differ.

So each master gets a generated hat. MD3 is a documented format and this tree
already generates 3D for the aura, so the mesh comes out of the build rather
than out of a modelling tool nobody here can run.

All five masters take krillin's head, and nappa's is not used, which is the
other half of the reported broken heads and worth stating plainly.

A head is joined to a body by tag_head, and CG_PositionRotatedEntityOnTag hands
the head that tag's axes - so a head mesh is only correct on a body whose tag
was authored for it. nappa's body hands its head (0,1,0) as forward; the bodies
oberak and tolm actually wear hand it (-0.46,0,-0.89). nappa's head on those
bodies is turned most of a half turn and pitched besides: a head on backwards,
which is what "does not look like a proper head" was describing. The tag axes
are per-frame and the two rigs disagree across the whole animation, so there is
no single corrective rotation to bake in. Moving those two masters to the donor
their bodies already agree with fixes it outright, and costs nothing now that a
silhouette comes from the gear rather than from the skull.

Fitting is still measured off the donor rather than hardcoded - skull centre,
radius and brow height from the vertices, facing from tag_eyes - because that
is what lets a sixth master take a different donor without retuning by hand.

Four of the five shapes are rotationally symmetric and need no facing at all.
Only the cowl has a front.

The gear is a second surface on a copy of the donor head, not a second model.
cg_master.c draws three models joined on tags and has no fourth slot, and the
shipped .skin mechanism already keys a shader per surface name, so a surface is
the seam that needs no engine change at all.

The donor head does not deform, so rigid gear is safe. add_surface repeats the
vertices per frame regardless, because the format requires it.

usage:
    make_headgear.py <players-dir> <scripts-dir> [--only MASTER] [--preview DIR]
"""
import argparse
import math
import os
import re
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import md3

S = md3.MD3_XYZ_SCALE
SEGS = 20                      # segments around a body of revolution
GEAR_SURFACE = "headgear"      # surface name the .skin keys a shader to


# ------------------------------------------------------------------ fitting

class Skull:
    """What a hat needs to know about the head it sits on."""

    def __init__(self, model):
        surf = model.surfaces[0]
        v = surf.frames[0]
        xs = [a[0] * S for a in v]
        ys = [a[1] * S for a in v]
        zs = [a[2] * S for a in v]
        self.top = max(zs)
        self.bottom = min(zs)
        mid = (self.top + self.bottom) * 0.5
        # The cranium, not the neck: the upper half is what a hat rests on, and
        # including the neck would put the centre and the radius both too low.
        upper = [i for i in range(len(v)) if zs[i] >= mid]
        # Bounding-box midpoint rather than centroid. The vertices are dense
        # over the face and sparse over the back of the skull, so a centroid is
        # dragged forward and every radius measured from it comes out too big.
        self.cx = (min(xs[i] for i in upper) + max(xs[i] for i in upper)) * 0.5
        self.cy = (min(ys[i] for i in upper) + max(ys[i] for i in upper)) * 0.5
        d = sorted(math.hypot(xs[i] - self.cx, ys[i] - self.cy) for i in upper)
        # A high percentile, not the maximum: one ear or nose vertex standing
        # proud of the skull should not set the size of the hat.
        self.r = d[int(len(d) * 0.85)]

        eyes = None
        fx, fy = 1.0, 0.0
        for nm, origin, _axis in (model.tags[0] if model.tags else []):
            if nm == "tag_eyes":
                eyes = origin
                fx, fy = origin[0] - self.cx, origin[1] - self.cy
                break
        # The brow, from where the eyes are authored rather than from a guess
        # off the crown: the two donors have different proportions and a fixed
        # fraction of the radius puts a band across one of the two faces.
        ez = eyes[2] if eyes else (self.top - self.r)
        self.brow = ez + 0.30 * (self.top - ez)
        self.cz = (self.top + self.brow) * 0.5
        n = math.hypot(fx, fy) or 1.0
        self.fx, self.fy = fx / n, fy / n

    def place(self, x, y, z):
        """Local (forward, left, up) into model space."""
        return (self.cx + x * self.fx - y * self.fy,
                self.cy + x * self.fy + y * self.fx,
                z)


# ------------------------------------------------------------------ meshing

class Mesh:
    def __init__(self):
        self.v = []
        self.uv = []
        self.t = []

    def add(self, p, uv):
        self.v.append(p)
        self.uv.append(uv)
        return len(self.v) - 1

    def quad(self, a, b, c, d):
        self.t.append((a, b, c))
        self.t.append((a, c, d))

    def revolve(self, profile, segs=SEGS, urep=1.0, phi0=0.0, phi1=2 * math.pi,
                closed=True):
        """A body of revolution from a list of (radius, z, v) rings."""
        n = segs if closed else segs + 1
        rings = []
        for r, z, tv in profile:
            row = []
            for i in range(n):
                f = i / float(segs)
                a = phi0 + (phi1 - phi0) * f
                row.append(self.add((r * math.cos(a), r * math.sin(a), z),
                                    (f * urep, tv)))
            rings.append(row)
        for k in range(len(rings) - 1):
            lo, hi = rings[k], rings[k + 1]
            for i in range(segs if closed else segs):
                j = (i + 1) % n if closed else i + 1
                self.quad(lo[i], lo[j], hi[j], hi[i])
        return rings

    def cap(self, ring, z, tv, segs=SEGS, flip=False):
        c = self.add((0, 0, z), (0.5, tv))
        for i in range(len(ring)):
            j = (i + 1) % len(ring)
            self.t.append((c, ring[j], ring[i]) if flip else (c, ring[i], ring[j]))

    def flip(self):
        self.t = [(a, c, b) for a, b, c in self.t]


def double(mesh):
    """Add the reverse of every triangle - a brim seen from below is not a
    back face, it is the underside of a hat."""
    mesh.t = mesh.t + [(a, c, b) for a, b, c in mesh.t]


# ------------------------------------------------------------------- shapes

def gear_kasa(s):
    """A wide conical straw hat. The brim is what carries at distance."""
    m = Mesh()
    R = s.r * 2.15
    brim_z = s.brow + s.r * 0.10
    apex_z = s.top + s.r * 0.55
    m.revolve([(R * 0.02, apex_z, 0.00),
               (R * 0.45, apex_z - (apex_z - brim_z) * 0.55, 0.22),
               (R * 0.78, brim_z + s.r * 0.22, 0.38),
               (R, brim_z, 0.50)], urep=2.0)
    # The underside, domed slightly up so the hat has a hollow rather than
    # being a paper cone.
    m.revolve([(R, brim_z, 0.52),
               (R * 0.55, brim_z + s.r * 0.14, 0.72),
               (s.r * 0.30, brim_z + s.r * 0.45, 0.92)], urep=2.0)
    return m, 0.0


def gear_turban(s):
    """Wrapped cloth: a bulging stack whose UV repeats so the wrap reads."""
    m = Mesh()
    prof = []
    steps = 7
    z0 = s.brow
    z1 = s.top + s.r * 0.16
    for i in range(steps + 1):
        t = i / float(steps)
        z = z0 + (z1 - z0) * t
        r = s.r * (1.02 + 0.16 * math.sin(math.pi * (t ** 0.9)))
        prof.append((r, z, t * 0.75))
    # Domed, not capped: a flat lid turns the wrap into a pillbox and loses the
    # one thing that says cloth rather than hat.
    last = prof[-1][0]
    for i in range(1, 4):
        t = i / 3.0
        ang = math.pi * 0.5 * t
        prof.append((last * math.cos(ang), z1 + s.r * 0.34 * math.sin(ang),
                     0.75 + 0.25 * t))
    m.revolve(prof, urep=4.0)
    return m, 0.0


def gear_cowl(s):
    """A hood: a dome with the face cut out of it, and an inner shell so the
    opening has thickness instead of showing the back of a single surface."""
    m = Mesh()
    open_half = math.radians(56.0)
    # The mesh's own +x is the facing after Skull.place, so the opening is
    # centred on angle 0 and the shell spans the rest of the circle.
    phi0, phi1 = open_half, 2 * math.pi - open_half
    for scale, tv0, flip in ((1.26, 0.0, False), (1.08, 0.5, True)):
        prof = []
        steps = 7
        for i in range(steps + 1):
            t = i / float(steps)
            # Past 90 degrees, so the hood comes down the sides and back of the
            # skull instead of capping it like a helmet.
            ang = math.radians(6.0 + 128.0 * t)
            r = s.r * scale * math.sin(ang)
            z = s.cz + s.r * scale * math.cos(ang)
            prof.append((r, z, tv0 + 0.5 * t))
        sub = Mesh()
        sub.revolve(prof, urep=1.0, phi0=phi0, phi1=phi1, closed=False)
        if flip:
            sub.flip()
        base = len(m.v)
        m.v += sub.v
        m.uv += sub.uv
        m.t += [(a + base, b + base, c + base) for a, b, c in sub.t]
    return m, 0.0


def gear_topknot(s):
    """A shaved head with a bound topknot: a low band and a bun on a stalk."""
    m = Mesh()
    band = [(s.r * 1.01, s.brow + s.r * 0.10, 0.05),
            (s.r * 1.01, s.brow + s.r * 0.24, 0.20)]
    m.revolve(band, urep=3.0)
    stalk = [(s.r * 0.20, s.top - s.r * 0.10, 0.30),
             (s.r * 0.16, s.top + s.r * 0.12, 0.42)]
    m.revolve(stalk, urep=1.5)
    # The bun, a squashed ball sitting straight on the stalk. It is the one
    # piece that has to read from behind, so it is generous.
    prof = []
    steps = 6
    cz = s.top + s.r * 0.46
    for i in range(steps + 1):
        t = i / float(steps)
        ang = math.pi * t
        prof.append((s.r * 0.40 * math.sin(ang),
                     cz + s.r * 0.34 * math.cos(ang),
                     0.55 + 0.45 * t))
    m.revolve(prof, urep=2.0)
    return m, 0.0


def gear_circlet(s):
    """A plain metal band with a raised boss at the brow - the least of the
    five on purpose, because one master should read as bare-headed."""
    m = Mesh()
    m.revolve([(s.r * 1.04, s.brow + s.r * 0.05, 0.10),
               (s.r * 1.07, s.brow + s.r * 0.17, 0.35),
               (s.r * 1.04, s.brow + s.r * 0.29, 0.60)], urep=3.0)
    # The boss, a small pyramid standing off the front of the band.
    z = s.brow + s.r * 0.17
    w = s.r * 0.20
    tip = m.add((s.r * 1.28, 0, z), (0.5, 0.92))
    ring = [m.add((s.r * 1.02, -w, z - w), (0.35, 0.75)),
            m.add((s.r * 1.02, w, z - w), (0.65, 0.75)),
            m.add((s.r * 1.02, w, z + w), (0.65, 0.99)),
            m.add((s.r * 1.02, -w, z + w), (0.35, 0.99))]
    for i in range(4):
        m.t.append((tip, ring[i], ring[(i + 1) % 4]))
    return m, 0.0


SHAPES = {
    "kasa": gear_kasa,
    "turban": gear_turban,
    "cowl": gear_cowl,
    "topknot": gear_topknot,
    "circlet": gear_circlet,
}

# One silhouette each. rhogan keeps the plainest gear because his face repaint
# is the strongest of the five and a hat would hide it.
MASTERS = {
    "oberak": ("kasa", "krillin"),
    "tolm": ("cowl", "krillin"),
    "seppa": ("turban", "krillin"),
    "naida": ("topknot", "krillin"),
    "rhogan": ("circlet", "krillin"),
}

DOUBLE_SIDED = ("kasa",)

# The surface name each donor mesh calls its head, which is what a .skin keys on.
DONOR_SURFACE = {"krillin": "Head", "nappa": "nappaHead"}


# -------------------------------------------------------------------- build

def build(players, master, shape, donor, preview=None):
    head = os.path.join(players, master, "tier1", "head.md3")
    src = os.path.join(players, donor, "tier1", "head.md3")
    if not os.path.exists(src):
        print("  %s: donor %s has no tier1/head.md3, skipped" % (master, donor))
        return None
    # Whatever head the master had before this pass is kept, per the standing
    # instruction to discard nothing.
    keep = head + ".predonor"
    if os.path.exists(head) and not os.path.exists(keep):
        shutil.copy2(head, keep)
    model = md3.load(src)

    # Drop any gear from an earlier run so this is re-runnable.
    model.surfaces = [s for s in model.surfaces if s.name != GEAR_SURFACE]

    skull = Skull(model)
    mesh, _ = SHAPES[shape](skull)
    if shape in DOUBLE_SIDED:
        double(mesh)

    surf = md3.Surface()
    surf.name = GEAR_SURFACE
    shader = "%sGear" % master
    surf.shaders = [(shader, 0)]
    surf.triangles = list(mesh.t)
    surf.st = list(mesh.uv)
    verts = []
    for (x, y, z) in mesh.v:
        wx, wy, wz = skull.place(x, y, z)
        verts.append((int(round(wx / S)), int(round(wy / S)), int(round(wz / S)),
                      md3.pack_normal(x, y, z - skull.cz)))
    surf.frames = [verts]
    md3.add_surface(model, surf)

    # The frame bounds gate culling, and a hat that sticks out past them gets
    # the whole head culled early at the screen edge.
    grew = []
    for mins, maxs, origin, radius, nm in model.frames:
        lo = list(mins)
        hi = list(maxs)
        rad = radius
        for (x, y, z) in mesh.v:
            p = skull.place(x, y, z)
            for k in range(3):
                lo[k] = min(lo[k], p[k])
                hi[k] = max(hi[k], p[k])
            rad = max(rad, math.sqrt(sum(c * c for c in p)))
        grew.append((tuple(lo), tuple(hi), origin, rad, nm))
    model.frames = grew

    md3.save(model, head)
    print("  %-8s %-8s from %-8s  %3d verts %3d tris  skull r=%.2f facing %+.2f %+.2f"
          % (master, shape, donor, len(mesh.v), len(mesh.t), skull.r,
             skull.fx, skull.fy))
    if preview:
        os.makedirs(preview, exist_ok=True)
    return shape


# ------------------------------------------------------------------ cloth
#
# The gear UVs run u around the shape and v up it, so a material is a vertical
# ramp with grain across it. Each master's cloth is a different weave as well as
# a different colour, because two masters in the same fabric at fifty metres are
# two masters in one hat.

CLOTH = {
    # base, shade, weave direction, weave pitch, grain strength
    "oberak": ((198, 168, 108), (150, 120, 68), "u", 3, 0.30),   # straw
    "tolm": ((78, 84, 104), (44, 48, 62), "v", 5, 0.18),         # coarse wool
    "seppa": ((150, 54, 48), (96, 30, 28), "u", 7, 0.22),        # wrapped silk
    "naida": ((44, 40, 46), (22, 20, 24), "v", 4, 0.16),         # dark cloth
    "rhogan": ((172, 132, 62), (108, 78, 34), "u", 11, 0.26),    # bronze
}


def make_cloth(master, path, size=256):
    from PIL import Image
    import random

    base, shade, axis, pitch, grain = CLOTH[master]
    rng = random.Random(hash(master) & 0xffff)
    img = Image.new("RGB", (size, size))
    px = img.load()
    for y in range(size):
        # v runs up the shape: a little darker at the bottom so the gear has
        # weight rather than reading as a flat decal.
        t = y / float(size - 1)
        lift = 0.82 + 0.18 * (1.0 - t)
        for x in range(size):
            k = (x if axis == "u" else y) // pitch
            w = 1.0 if k % 2 else 1.0 - grain
            n = 1.0 + rng.uniform(-0.05, 0.05)
            c = []
            for i in range(3):
                v = (base[i] * w + shade[i] * (1 - w)) * lift * n
                c.append(max(0, min(255, int(v))))
            px[x, y] = tuple(c)
    img.save(path)
    return path


# ------------------------------------------------- shader and skin wiring
#
# Read and written as bytes, and the file's own line ending is reused. The
# shaders and skins in this tree ship CRLF and a text-mode rewrite converts the
# whole file silently - see CLAUDE.md.

GEAR_SHADER = """
// Generated headgear - see foundry/tools/make_headgear.py. One surface added
// to the donor head, so the silhouette differs before the paint does.
%(master)sGear
{
\toutlines
\tnomipmaps
\t{
\t\tmap players/%(master)s/%(master)sGear.png
\t\trgbGen identityLighting
\t}
\t{
\t\tclampMap effects/shading/celShadeDark3Tone.png
\t\tblendfunc filter
\t\trgbGen lightingUniform
\t\ttcGen cel
\t}
}
"""


def _eol(data):
    return b"\r\n" if b"\r\n" in data else b"\n"


def wire_shader(scripts, master):
    path = os.path.join(scripts, "player%s.shader" % (master[0].upper() + master[1:]))
    if not os.path.exists(path):
        print("    no shader file at %s" % path)
        return
    data = open(path, "rb").read()
    name = ("%sGear" % master).encode()
    if re.search(br"(?m)^%s\s*$" % re.escape(name), data):
        return
    eol = _eol(data)
    block = (GEAR_SHADER % {"master": master}).replace("\n", eol.decode("latin-1"))
    if not data.endswith(eol):
        data += eol
    open(path, "wb").write(data + block.encode("latin-1"))


def wire_skins(players, master, donor_surface):
    """Add the gear line, and key the head line to the donor's surface name.

    A .skin maps SURFACE NAME to shader, so swapping the head mesh for another
    donor's silently unskins the head unless the key is swapped with it: the
    old line named nappaHead and the krillin mesh calls its surface Head, so
    the head would fall back to the shader baked into the md3.
    """
    tier = os.path.join(players, master, "tier1")
    line = ("%s,%sGear" % (GEAR_SURFACE, master)).encode()
    surf = donor_surface.encode()
    for fn in sorted(os.listdir(tier)):
        if not fn.endswith(".skin"):
            continue
        path = os.path.join(tier, fn)
        data = open(path, "rb").read()
        eol = _eol(data)
        keep = path + ".preheadgear"
        if not os.path.exists(keep):
            shutil.copy2(path, keep)
        # Re-key whichever head surface this skin currently names.
        data = re.sub(br"(?mi)^[ \t]*(nappaHead|Head)[ \t]*,", surf + b",", data)
        if line not in data:
            if data and not data.endswith(eol):
                data += eol
            data += line + eol
        open(path, "wb").write(data)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("players")
    ap.add_argument("scripts", nargs="?")
    ap.add_argument("--only")
    ap.add_argument("--preview")
    a = ap.parse_args()
    print("headgear:")
    for master, (shape, donor) in MASTERS.items():
        if a.only and master != a.only:
            continue
        if build(a.players, master, shape, donor, a.preview) is None:
            continue
        make_cloth(master, os.path.join(a.players, master, "%sGear.png" % master))
        wire_skins(a.players, master, DONOR_SURFACE[donor])
        if a.scripts:
            wire_shader(a.scripts, master)


if __name__ == "__main__":
    main()
