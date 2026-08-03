#!/usr/bin/env python3
"""Render an MD3 with its sheet, offline, so a repaint can be judged before the
engine is involved.

Not a renderer for anything but this job: flat texture lookup, z-buffer, one
headlight term, no cel band and no outline pass. That is deliberate - it shows
what the SHEET does to the MESH, which is the thing being iterated on, and
leaves the shader's contribution out of the picture.

usage: md3_preview.py <model.md3> <sheet.png> <out.png> [--yaw D] [--frame N]
                      [--size N] [--island]
"""
import argparse
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import md3
import numpy as np
from PIL import Image

BG = (28, 30, 36)


def render(model_path, sheet_path, out_path, yaw=0.0, pitch=8.0, frame=0,
           size=512, island=False):
    m = md3.load(model_path)
    tex = np.asarray(Image.open(sheet_path).convert("RGB")).astype(np.float32)
    th, tw = tex.shape[:2]

    tris = []
    for si, s in enumerate(m.surfaces):
        f = min(frame, len(s.frames) - 1)
        verts = s.frames[f]
        for a, b, c in s.triangles:
            tris.append((si, [verts[a], verts[b], verts[c]],
                         [s.st[a], s.st[b], s.st[c]]))

    pts = np.array([[v[0], v[1], v[2]] for _si, vs, _st in tris for v in vs],
                   dtype=np.float32) * md3.MD3_XYZ_SCALE
    lo, hi = pts.min(axis=0), pts.max(axis=0)
    ctr = (lo + hi) * 0.5
    span = float(np.max(hi - lo)) or 1.0

    cy, sy = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))
    cp, sp = math.cos(math.radians(pitch)), math.sin(math.radians(pitch))

    def project(v):
        x, y, z = (np.array(v[:3], dtype=np.float32) * md3.MD3_XYZ_SCALE) - ctr
        # model space is x forward, y left, z up; view down -x after the yaw
        rx, ry = x * cy - y * sy, x * sy + y * cy
        rz, rx2 = z * cp - rx * sp, z * sp + rx * cp
        k = size / (span * 1.35)
        return (size * 0.5 + ry * k, size * 0.5 - rz * k, rx2)

    colour = np.full((size, size, 3), BG, dtype=np.float32)
    zbuf = np.full((size, size), -1e9, dtype=np.float32)
    palette = [(230, 90, 90), (90, 220, 140), (110, 170, 255), (245, 210, 80)]

    for si, vs, sts in tris:
        p = [project(v) for v in vs]
        (x0, y0, d0), (x1, y1, d1), (x2, y2, d2) = p
        area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0)
        if abs(area) < 1e-6:
            continue
        e0 = np.array([vs[1][i] - vs[0][i] for i in range(3)], dtype=np.float32)
        e1 = np.array([vs[2][i] - vs[0][i] for i in range(3)], dtype=np.float32)
        nrm = np.cross(e0, e1)
        nl = np.linalg.norm(nrm)
        shade = 1.0
        if nl > 1e-9:
            nrm = nrm / nl
            vx = np.array([cy * cp, -sy * cp, sp], dtype=np.float32)
            shade = 0.45 + 0.55 * abs(float(np.dot(nrm, vx)))

        xmin = max(0, int(math.floor(min(x0, x1, x2))))
        xmax = min(size - 1, int(math.ceil(max(x0, x1, x2))))
        ymin = max(0, int(math.floor(min(y0, y1, y2))))
        ymax = min(size - 1, int(math.ceil(max(y0, y1, y2))))
        if xmax < xmin or ymax < ymin:
            continue
        xs = np.arange(xmin, xmax + 1) + 0.5
        ys = np.arange(ymin, ymax + 1) + 0.5
        gx, gy = np.meshgrid(xs, ys)
        w0 = ((x1 - gx) * (y2 - gy) - (x2 - gx) * (y1 - gy)) / area
        w1 = ((x2 - gx) * (y0 - gy) - (x0 - gx) * (y2 - gy)) / area
        w2 = 1.0 - w0 - w1
        inside = (w0 >= 0) & (w1 >= 0) & (w2 >= 0)
        if not inside.any():
            continue
        depth = w0 * d0 + w1 * d1 + w2 * d2
        sub = zbuf[ymin:ymax + 1, xmin:xmax + 1]
        win = inside & (depth > sub)
        if not win.any():
            continue
        if island:
            src = np.array(palette[si % len(palette)], dtype=np.float32)
            px = np.broadcast_to(src, w0.shape + (3,)).copy()
        else:
            u = w0 * sts[0][0] + w1 * sts[1][0] + w2 * sts[2][0]
            v = w0 * sts[0][1] + w1 * sts[1][1] + w2 * sts[2][1]
            ui = np.clip((u * tw).astype(np.int32), 0, tw - 1)
            vi = np.clip((v * th).astype(np.int32), 0, th - 1)
            px = tex[vi, ui]
        px = px * shade
        sub[win] = depth[win]
        tgt = colour[ymin:ymax + 1, xmin:xmax + 1]
        tgt[win] = px[win]

    Image.fromarray(np.clip(colour, 0, 255).astype(np.uint8)).save(out_path)
    return out_path


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("model")
    ap.add_argument("sheet")
    ap.add_argument("out")
    ap.add_argument("--yaw", type=float, default=0.0)
    ap.add_argument("--pitch", type=float, default=8.0)
    ap.add_argument("--frame", type=int, default=0)
    ap.add_argument("--size", type=int, default=512)
    ap.add_argument("--island", action="store_true")
    a = ap.parse_args()
    print(render(a.model, a.sheet, a.out, a.yaw, a.pitch, a.frame, a.size, a.island))
