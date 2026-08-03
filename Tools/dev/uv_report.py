#!/usr/bin/env python3
"""Draw a head's UV wireframe over the sheet painted onto it.

The point is to see which parts of a 512x512 sheet are actually reachable and
which triangle sits where, because a repaint done by eye on the flat image will
happily paint a stripe across islands that are nowhere near each other on the
model.

usage: uv_report.py <model.md3> <sheet.png> <out.png> [surface]
"""
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import md3
from PIL import Image, ImageDraw


def report(model_path, sheet_path, out_path, want=None):
    m = md3.load(model_path)
    sheet = Image.open(sheet_path).convert("RGB")
    W, H = sheet.size
    scale = 2
    img = sheet.resize((W * scale, H * scale), Image.NEAREST).convert("RGBA")
    over = Image.new("RGBA", img.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(over)

    print("model %s: %d frames, %d surfaces" % (
        os.path.basename(model_path), len(m.frames), len(m.surfaces)))
    for s in m.surfaces:
        print("  surface %-24s verts=%-5d tris=%-5d shaders=%s" % (
            s.name, s.numVerts, len(s.triangles), [n for n, _ in s.shaders]))

    colours = [(255, 60, 60), (60, 255, 120), (80, 160, 255), (255, 220, 40),
               (255, 100, 255), (40, 255, 255)]
    for si, s in enumerate(m.surfaces):
        if want and s.name != want:
            continue
        c = colours[si % len(colours)] + (200,)
        us = [(u * W * scale, v * H * scale) for u, v in s.st]
        for a, b, cc in s.triangles:
            d.line([us[a], us[b], us[cc], us[a]], fill=c, width=1)
        xs = [p[0] for p in us]
        ys = [p[1] for p in us]
        if xs:
            print("  %-24s u=[%.3f..%.3f] v=[%.3f..%.3f]" % (
                s.name, min(xs) / (W * scale), max(xs) / (W * scale),
                min(ys) / (H * scale), max(ys) / (H * scale)))

    Image.alpha_composite(img, over).convert("RGB").save(out_path)
    print("wrote", out_path)


if __name__ == "__main__":
    report(sys.argv[1], sys.argv[2], sys.argv[3],
           sys.argv[4] if len(sys.argv) > 4 else None)
