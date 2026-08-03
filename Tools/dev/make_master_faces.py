#!/usr/bin/env python3
"""Repaint the masters' face sheets in terms of the head, not the sheet.

This replaces a repaint that was done by eye on the flat image, and the reason
it looked mangled is worth stating because it is the whole design of this tool.

The donor heads unwrap radially: the top quarter of the sheet is the CROWN,
splayed out, and the middle is the face. A "headband" painted as a horizontal
stripe across the sheet - which is what the previous pass did - therefore lands
on the top of the skull and wraps as a gold cap with a hard edge round it. The
second fault was tonal: the shipped face is thin dark linework over light skin,
so flooding the face with a dark brown left the features with no contrast and
the head read as a smooth mass with two eyes.

So nothing here is painted in sheet coordinates. The mesh's UV triangles are
rasterised once into a map of model-space position and normal per texel, and
every mark is then placed by where it is ON THE HEAD - brow ridge, jaw, cheek -
which is correct on both donors even though one faces +x and the other -y and
their layouts have nothing in common.

Contrast is kept by construction rather than by care: the authored linework is
extracted from the original sheet as a ratio against its own blur and
multiplied back over whatever new skin tone is chosen, so recolouring cannot
flatten features that were drawn dark-on-light.

Beards are strokes along the hair's direction of growth at sheet resolution,
not a smudge: the same treatment the shipped sheets give Goku's hair.

The originals are never overwritten - every output is a new file beside them.

usage:
    make_master_faces.py <players-dir> [--only MASTER]
"""
import argparse
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import md3
import numpy as np
from PIL import Image, ImageFilter

S = md3.MD3_XYZ_SCALE


# ------------------------------------------------------- geometry into UV

def surface_maps(model, size, surface=None):
    """Rasterise UV triangles into per-texel model position, normal and mask."""
    pos = np.zeros((size, size, 3), np.float32)
    nrm = np.zeros((size, size, 3), np.float32)
    mask = np.zeros((size, size), bool)

    for surf in model.surfaces:
        if surface and surf.name != surface:
            continue
        v = surf.frames[0]
        P = np.array([[a[0] * S, a[1] * S, a[2] * S] for a in v], np.float32)
        for a, b, c in surf.triangles:
            uv = [surf.st[i] for i in (a, b, c)]
            xy = [(u * size, t * size) for u, t in uv]
            (x0, y0), (x1, y1), (x2, y2) = xy
            area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0)
            if abs(area) < 1e-9:
                continue
            e0, e1 = P[b] - P[a], P[c] - P[a]
            fn = np.cross(e0, e1)
            ln = np.linalg.norm(fn)
            fn = fn / ln if ln > 1e-9 else np.array([0, 0, 1], np.float32)
            xmin = max(0, int(min(x0, x1, x2)) - 1)
            xmax = min(size - 1, int(max(x0, x1, x2)) + 1)
            ymin = max(0, int(min(y0, y1, y2)) - 1)
            ymax = min(size - 1, int(max(y0, y1, y2)) + 1)
            if xmax < xmin or ymax < ymin:
                continue
            gx, gy = np.meshgrid(np.arange(xmin, xmax + 1) + 0.5,
                                 np.arange(ymin, ymax + 1) + 0.5)
            w0 = ((x1 - gx) * (y2 - gy) - (x2 - gx) * (y1 - gy)) / area
            w1 = ((x2 - gx) * (y0 - gy) - (x0 - gx) * (y2 - gy)) / area
            w2 = 1.0 - w0 - w1
            # A hair of slop, so texels straddling a shared edge are covered by
            # one of the two triangles rather than by neither.
            ins = (w0 >= -0.02) & (w1 >= -0.02) & (w2 >= -0.02)
            if not ins.any():
                continue
            p = (w0[..., None] * P[a] + w1[..., None] * P[b] + w2[..., None] * P[c])
            sub_p = pos[ymin:ymax + 1, xmin:xmax + 1]
            sub_n = nrm[ymin:ymax + 1, xmin:xmax + 1]
            sub_m = mask[ymin:ymax + 1, xmin:xmax + 1]
            sub_p[ins] = p[ins]
            sub_n[ins] = fn
            sub_m[ins] = True
    return pos, nrm, mask


class Frame:
    """Head-relative coordinates: forward, left, up, all in skull radii."""

    def __init__(self, model, pos, mask):
        v = model.surfaces[0].frames[0]
        xs = [a[0] * S for a in v]
        ys = [a[1] * S for a in v]
        zs = [a[2] * S for a in v]
        top, bot = max(zs), min(zs)
        mid = (top + bot) * 0.5
        up = [i for i in range(len(v)) if zs[i] >= mid]
        cx = (min(xs[i] for i in up) + max(xs[i] for i in up)) * 0.5
        cy = (min(ys[i] for i in up) + max(ys[i] for i in up)) * 0.5
        d = sorted(math.hypot(xs[i] - cx, ys[i] - cy) for i in up)
        r = d[int(len(d) * 0.85)]
        eyes = None
        fx, fy = 1.0, 0.0
        for nm, o, _a in (model.tags[0] if model.tags else []):
            if nm == "tag_eyes":
                eyes = o
                fx, fy = o[0] - cx, o[1] - cy
                break
        n = math.hypot(fx, fy) or 1.0
        fx, fy = fx / n, fy / n
        self.r, self.top = r, top
        self.eyez = eyes[2] if eyes else top - r

        dx = pos[..., 0] - cx
        dy = pos[..., 1] - cy
        self.fwd = (dx * fx + dy * fy) / r          # + is toward the face
        self.left = (-dx * fy + dy * fx) / r        # + is the head's left
        self.up = (pos[..., 2] - self.eyez) / r     # 0 at the eyes
        self.mask = mask


# ----------------------------------------------------------------- paint

def detail_layer(img):
    """The authored linework, as a ratio against its own blur.

    Multiplying this back over a new skin tone keeps every drawn feature at the
    contrast it was drawn with, which is what a flat recolour destroys.

    Darken-only. The ratio runs above 1 just outside every dark stroke - the
    blur is lighter there than the pixel - and letting that through rings each
    brow with a bright halo, which on the model reads as the paint glowing.
    These sheets are dark linework over flat skin and have no highlights to
    preserve, so clamping at 1 costs nothing and removes the artefact.
    """
    g = img.convert("L")
    blur = g.filter(ImageFilter.GaussianBlur(9))
    a = np.asarray(g, np.float32) + 1.0
    b = np.asarray(blur, np.float32) + 1.0
    return np.clip(a / b, 0.30, 1.0)


def strokes(shape, seed, angle, density, length):
    """Directional hair strokes at sheet resolution."""
    rng = np.random.default_rng(seed)
    out = np.zeros(shape, np.float32)
    h, w = shape
    n = int(h * w * density / 1000.0)
    ca, sa = math.cos(angle), math.sin(angle)
    ys = rng.integers(0, h, n)
    xs = rng.integers(0, w, n)
    lens = rng.integers(length // 2, length + 1, n)
    jit = rng.normal(0, 0.22, n)
    for k in range(n):
        c, s = math.cos(angle + jit[k]), math.sin(angle + jit[k])
        for t in range(lens[k]):
            y = int(ys[k] + s * t)
            x = int(xs[k] + c * t)
            if 0 <= y < h and 0 <= x < w:
                out[y, x] = max(out[y, x], 1.0 - t / float(lens[k] + 2))
    del ca, sa
    return out


def band(v, lo, hi, soft):
    """A soft membership in [lo,hi]."""
    return (np.clip((v - lo) / soft, 0, 1) * np.clip((hi - v) / soft, 0, 1))


def paint(master, spec, model, sheet_path, out_path):
    img = Image.open(sheet_path).convert("RGB")
    size = img.size[0]
    pos, nrm, mask = surface_maps(model, size, model.surfaces[0].name)
    f = Frame(model, pos, mask)
    det = detail_layer(img)

    # The donor's own identifying marks, erased where they sit ON THE HEAD.
    # Krillin's six forehead dots are the strongest tell either donor carries -
    # a viewer names him from those before anything else - and they survive a
    # recolour because they are linework and the detail layer is what protects
    # linework. So the detail is neutralised over the forehead and the age
    # lines this tool draws go back on top.
    for lo, hi, half in spec.get("erase", ()):
        area = band(f.up, lo, hi, 0.22) * np.clip(f.fwd * 1.6, 0, 1)
        area = area * np.clip((half - np.abs(f.left)) / 0.25, 0, 1)
        det = det * (1 - area) + area

    skin = np.array(spec["skin"], np.float32)
    base = np.repeat(np.repeat(skin[None, None, :], size, 0), size, 1)

    # Cheeks and temples a touch darker than the brow and nose ridge, so the
    # face has form under the linework rather than being a flat fill.
    form = 1.0 - 0.10 * np.clip(np.abs(f.left) - 0.35, 0, 1)
    form = form - 0.06 * np.clip(-f.fwd, 0, 1)
    base = base * form[..., None]

    face = np.clip(f.fwd, 0, 1)          # how much a texel faces forward

    # --- beard -----------------------------------------------------------
    if spec.get("beard"):
        lo, hi, ang, dens, ln, col = spec["beard"]
        area = band(f.up, lo, hi, 0.28) * np.clip(face * 1.4, 0, 1)
        area = area * np.clip(1.0 - np.abs(f.left) * spec.get("beardWidth", 0.75), 0, 1)
        st = strokes((size, size), spec["seed"], ang, dens, ln)
        a = np.clip(area * (0.45 + 0.55 * st), 0, 1)
        base = base * (1 - a[..., None]) + np.array(col, np.float32) * a[..., None]

    # --- brows -----------------------------------------------------------
    if spec.get("brow"):
        z, thick, col = spec["brow"]
        area = band(f.up, z - thick, z + thick, 0.10) * np.clip(face * 1.6, 0, 1)
        area = area * np.clip(1.0 - np.clip(np.abs(f.left) - 0.55, 0, 1) * 3.0, 0, 1)
        st = strokes((size, size), spec["seed"] + 7, 0.0, 26, 5)
        a = np.clip(area * (0.55 + 0.45 * st), 0, 1) * spec.get("browStrength", 0.9)
        base = base * (1 - a[..., None]) + np.array(col, np.float32) * a[..., None]

    # --- age lines -------------------------------------------------------
    if spec.get("lines"):
        n, strength = spec["lines"]
        acc = np.zeros((size, size), np.float32)
        for i in range(n):
            z = 0.44 + 0.15 * i
            acc = np.maximum(acc, band(f.up, z - 0.018, z + 0.018, 0.030))
        acc = acc * np.clip(face * 1.5, 0, 1)
        acc = acc * np.clip(1.0 - np.clip(np.abs(f.left) - 0.42, 0, 1) * 3.0, 0, 1)
        base = base * (1 - (acc * strength)[..., None] * 0.55)

    # --- one mark: a scar or a tattoo ------------------------------------
    if spec.get("mark"):
        kind, side, col = spec["mark"]
        if kind == "scar":
            # A diagonal across one brow and cheek.
            t = (f.up - 0.30) - 0.85 * (f.left * side - 0.30)
            a = band(t, -0.030, 0.030, 0.035) * np.clip(face * 1.6, 0, 1)
            a = a * band(f.left * side, 0.02, 1.05, 0.25)
        else:
            # A crescent on the temple.
            rr = np.sqrt((f.left * side - 0.72) ** 2 + (f.up - 0.42) ** 2)
            a = band(rr, 0.14, 0.24, 0.06) * np.clip(face * 1.3, 0, 1)
        a = np.clip(a, 0, 1) * 0.85
        base = base * (1 - a[..., None]) + np.array(col, np.float32) * a[..., None]

    out = base * det[..., None]

    # Off-mesh texels keep the original sheet: unreachable padding is not worth
    # inventing, and the seam bleed the shipped art relies on lives there.
    orig = np.asarray(img, np.float32)
    m3 = np.repeat(mask[..., None], 3, 2)
    out = np.where(m3, out, orig)
    Image.fromarray(np.clip(out, 0, 255).astype(np.uint8)).save(out_path)
    return out_path


# --------------------------------------------------------------- the cast
#
# Five faces that have to be five people. Skin tone alone was tried and was not
# enough, so each also gets a different arrangement of hair: a full beard, a
# jaw-line beard, a moustache only, clean-shaven with heavy brows, and a
# weathered face with neither.

MASTERS = {
    "oberak": dict(
        donor="krillin", skin=(196, 150, 116), seed=11,
        beard=(-1.30, -0.10, 1.35, 210, 13, (58, 44, 36)), beardWidth=0.62,
        brow=(0.30, 0.10, (48, 36, 30)), browStrength=1.0,
        lines=(3, 0.9), mark=("scar", 1.0, (170, 122, 100))),
    "tolm": dict(
        donor="krillin", skin=(168, 126, 96), seed=23,
        beard=(-1.15, -0.45, 1.55, 150, 10, (72, 66, 62)), beardWidth=0.55,
        brow=(0.28, 0.085, (70, 62, 58)), browStrength=0.85,
        lines=(4, 1.0), mark=("tattoo", -1.0, (96, 70, 120))),
    "seppa": dict(
        donor="krillin", skin=(214, 172, 132), seed=37,
        beard=(-0.62, -0.28, 1.45, 120, 8, (52, 40, 34)), beardWidth=1.10,
        brow=(0.26, 0.075, (54, 40, 32)), browStrength=0.8,
        lines=(2, 0.6), mark=None, erase=((0.16, 1.15, 0.46),)),
    "naida": dict(
        donor="krillin", skin=(226, 190, 156), seed=53,
        beard=None,
        brow=(0.24, 0.060, (66, 48, 38)), browStrength=0.7,
        lines=(2, 0.5), mark=("tattoo", 1.0, (150, 60, 70)),
        erase=((0.16, 1.15, 0.46),)),
    "rhogan": dict(
        donor="krillin", skin=(184, 138, 102), seed=71,
        beard=(-1.05, 0.02, 1.30, 230, 15, (40, 34, 30)), beardWidth=0.58,
        brow=(0.31, 0.115, (34, 28, 24)), browStrength=1.0,
        lines=(4, 1.0), mark=("scar", -1.0, (156, 108, 88)),
        erase=((0.16, 1.15, 0.46),)),
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("players")
    ap.add_argument("--only")
    a = ap.parse_args()
    print("faces:")
    for master, spec in MASTERS.items():
        if a.only and master != a.only:
            continue
        head = os.path.join(a.players, master, "tier1", "head.md3")
        keep = head + ".preheadgear"
        model = md3.load(keep if os.path.exists(keep) else head)
        donor = spec["donor"]
        src = os.path.join(a.players, donor,
                           "nappahead.png" if donor == "nappa" else "krillinHead.png")
        out = os.path.join(a.players, master, "%sHeadKit.png" % master)
        paint(master, spec, model, src, out)
        print("  %-8s from %-8s -> %s" % (master, donor, os.path.basename(out)))


if __name__ == "__main__":
    main()
