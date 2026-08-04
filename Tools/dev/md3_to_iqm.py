#!/usr/bin/env python3
"""Build a skeletal IQM character out of a ZEQ2 three-part md3 set.

A ZEQ2 character is lower.md3, upper.md3 and head.md3 joined per frame on
tag_torso and tag_head, with one .skin file over all three. This produces the
same character as one IQM entity on one skeleton, so that gear can hang off a
bone instead of being baked into a head mesh, and so that a game module can be
handed a bone to drive.

WHAT THIS DOES AND DOES NOT RECOVER - read this before trusting the output.

md3 is vertex animation: every vertex has its own position in every frame. A
skeleton cannot represent that in general. What this converter recovers is the
*rigid* part of the motion, which for this asset set is the joint chain the
md3s were already assembled with:

  - lower, upper and head each become one bone, bound rigidly, with their
    vertices frozen at one reference frame;
  - the per-frame tag chain becomes the bones' animation, so the torso still
    twists over the hips and the head still turns with the torso, frame for
    frame, exactly as the md3 assembly did;
  - every md3 tag becomes a bone of its own carrying that tag's model-space
    transform, so trap_R_LerpTag keeps working by name - R_IQMLerpTag matches
    joints by name and this writer binds every joint at the identity, which is
    the condition under which the matrix it returns is the joint's transform
    rather than a skinning matrix.

What is lost is everything *inside* a part: legs that stride, arms that swing,
a cape that flaps. Those vertices sit still. For a master standing at his mark
that is the whole animation and the loss is nil; for a fighter mid-combo it is
most of it. --report prints the per-frame RMS of exactly that residual so the
decision is made on a number rather than on a hunch.

usage:
    md3_to_iqm.py <playerdir> <out.iqm> [--tier tier1] [--ref-anim IDLE]
    md3_to_iqm.py <playerdir> --report [--tier tier1]
"""

import argparse
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import iqm                                              # noqa: E402
import md3                                              # noqa: E402

# The bones the meshes bind to. Names are the part names rather than anatomical
# ones because that is exactly what they are - one bone per md3, no more.
BONE_ROOT = "root"
BONE_LOWER = "lower"
BONE_UPPER = "upper"
BONE_HEAD = "head"
BONE_GEAR = "gear"

# The head surface an earlier pass merged into head.md3. Splitting it back onto
# its own bone is the point of Phase A: gear that is a bone away from the head
# can be swapped, hidden or aimed, and gear baked into the head mesh cannot.
GEAR_SURFACE = "headgear"


def parse_animation_cfg(path):
    """first frame / num frames / looping / fps rows, in file order.

    Only enough of the format to find a reference frame: the leading keyword
    lines (sex, fixedlegs, footsteps...) are skipped and every row of four or
    five integers is taken in order. cgame's own parser assigns them to the
    animNumber_t enum by position, and nothing here needs to know which is
    which beyond the count.
    """
    rows = []
    try:
        with open(path, "r", errors="replace") as fh:
            lines = fh.readlines()
    except OSError:
        return rows
    for line in lines:
        line = line.split("//", 1)[0].strip()
        if not line:
            continue
        parts = line.split()
        if len(parts) < 4:
            continue
        try:
            rows.append([int(float(p)) for p in parts[:4]])
        except ValueError:
            continue
    return rows


# animation.cfg is positional; ANIM_IDLE is the sixth row in the shipped files
# (three deaths, a stun, a floor recover, then idle). Stated as an index rather
# than searched for, because the comment naming it is a comment.
IDLE_ROW = 5


def tag_map(model, frame):
    """name -> 3x4 model-space transform for one md3 frame."""
    out = {}
    if not model.tags:
        return out
    row = model.tags[min(frame, len(model.tags) - 1)]
    for name, origin, axis in row:
        out[name] = iqm.mat_from_axis_origin(axis, origin)
    return out


def part_vertices(surface, frame):
    """Decoded (position, normal) for one surface at one frame."""
    verts = surface.frames[min(frame, len(surface.frames) - 1)]
    out = []
    for x, y, z, n in verts:
        out.append(((x * md3.MD3_XYZ_SCALE, y * md3.MD3_XYZ_SCALE,
                     z * md3.MD3_XYZ_SCALE), md3.unpack_normal(n)))
    return out


class Character:
    """The three md3s and the frame count they agree on."""

    def __init__(self, playerdir, tier):
        base = os.path.join(playerdir, tier)
        self.dir = playerdir
        self.lower = md3.load(os.path.join(base, "lower.md3"))
        self.upper = md3.load(os.path.join(base, "upper.md3"))
        self.head = md3.load(os.path.join(base, "head.md3"))
        self.frames = min(len(self.lower.frames), len(self.upper.frames))
        if self.frames < 1:
            raise ValueError("%s has no frames" % playerdir)
        self.anims = parse_animation_cfg(os.path.join(playerdir, "animation.cfg"))

    def reference_frame(self):
        """The frame the rigid parts are frozen at.

        The idle's first frame, not frame 0: frame 0 of every shipped rig is a
        death pose, and freezing the arms there gives a master who stands at
        his mark with his limbs where a corpse's would be.
        """
        if len(self.anims) > IDLE_ROW:
            f = self.anims[IDLE_ROW][0]
            if 0 <= f < self.frames:
                return f
        return 0

    def torso(self, frame):
        return tag_map(self.lower, frame).get("tag_torso", iqm.IDENTITY)

    def head_at(self, frame):
        """Model-space transform of the head part at one frame."""
        t = self.torso(frame)
        h = tag_map(self.upper, frame).get("tag_head", iqm.IDENTITY)
        return iqm.mat_mul(t, h)


def build(char, ref):
    """Assemble the iqm.Model. ref is the frame the meshes are frozen at."""
    model = iqm.Model()
    model.anim_name = "md3"
    model.framerate = 20.0

    j_root = model.add_joint(BONE_ROOT, -1)
    j_lower = model.add_joint(BONE_LOWER, j_root)
    j_upper = model.add_joint(BONE_UPPER, j_lower)
    j_head = model.add_joint(BONE_HEAD, j_upper)
    j_gear = model.add_joint(BONE_GEAR, j_head)

    # Tag bones. A name is claimed once, lower first: both lower and upper
    # carry a tag_torso (the joint seen from either side) and both upper and
    # head carry a tag_head, and the pair are the same place in model space, so
    # the duplicate would be a second bone at the same transform.
    tags = []                       # (jointIndex, owner, tagName)
    claimed = set()
    for owner, parent in (("lower", j_lower), ("upper", j_upper),
                          ("head", j_head)):
        part = getattr(char, owner)
        if not part.tags:
            continue
        for name, _origin, _axis in part.tags[0]:
            if name in claimed:
                continue
            claimed.add(name)
            tags.append((model.add_joint(name, parent), owner, name))

    # --- meshes, baked into model space at the reference frame ------------
    ref_torso = char.torso(ref)
    ref_head = char.head_at(ref)
    parts = (("lower", char.lower, iqm.IDENTITY, j_lower, ref),
             ("upper", char.upper, ref_torso, j_upper, ref),
             ("head", char.head, ref_head, j_head, 0))
    for _owner, part, place, bone, frame in parts:
        for surf in part.surfaces:
            target = j_gear if surf.name.lower() == GEAR_SURFACE else bone
            mesh = iqm.Mesh(surf.name, surf.shaders[0][0] if surf.shaders else "")
            for (p, n), uv in zip(part_vertices(surf, frame), surf.st):
                mesh.add_vertex(iqm.mat_transform(place, p), uv,
                                iqm.mat_rotate(place, n),
                                (target, 0, 0, 0), (255, 0, 0, 0))
            mesh.triangles = [tuple(t) for t in surf.triangles]
            model.meshes.append(mesh)

    # --- the pose track ---------------------------------------------------
    inv_ref_torso = iqm.mat_invert_rigid(ref_torso)
    inv_ref_head = iqm.mat_invert_rigid(ref_head)
    for f in range(char.frames):
        torso = char.torso(f)
        head = char.head_at(f)
        g = [None] * len(model.joints)
        g[j_root] = iqm.IDENTITY
        g[j_lower] = iqm.IDENTITY
        g[j_upper] = iqm.mat_mul(torso, inv_ref_torso)
        g[j_head] = iqm.mat_mul(head, inv_ref_head)
        g[j_gear] = g[j_head]
        lower_tags = tag_map(char.lower, f)
        upper_tags = tag_map(char.upper, f)
        head_tags = tag_map(char.head, 0)
        for index, owner, name in tags:
            if owner == "lower":
                g[index] = lower_tags.get(name, iqm.IDENTITY)
            elif owner == "upper":
                g[index] = iqm.mat_mul(torso, upper_tags.get(name, iqm.IDENTITY))
            else:
                g[index] = iqm.mat_mul(head, head_tags.get(name, iqm.IDENTITY))
        # Poses are stated relative to the parent; the bind is the identity, so
        # a joint's own global transform goes in unchanged at the root.
        frame = []
        for i, joint in enumerate(model.joints):
            if joint.parent < 0:
                frame.append(g[i])
            else:
                frame.append(iqm.mat_mul(iqm.mat_invert_rigid(g[joint.parent]),
                                         g[i]))
        model.frames.append(frame)
    return model


def report(char, ref, frames=None):
    """RMS of what the rigid bind cannot reproduce, in world units.

    Each part's true md3 vertices at a frame are compared against the same
    vertices frozen at the reference frame. That difference is exactly what the
    bone cannot carry: the bone reproduces the part's rigid placement from the
    tag chain, and everything left over is deformation inside the part. A ZEQ2
    fighter is about 56 units tall, so the number reads directly as how far a
    vertex sits from where the md3 puts it.

    It is an upper bound on the error rather than the tightest one - allowing
    each part's bone an independently best-fitting rotation per frame would
    absorb some of it - but not by an order of magnitude, because the tag chain
    already supplies the part's rotation.
    """
    out = []
    span = frames if frames is not None else range(char.frames)
    for owner, part in (("lower", char.lower), ("upper", char.upper)):
        base = [v for surf in part.surfaces for v in part_vertices(surf, ref)]
        n = len(base)
        worst = 0.0
        total = 0.0
        count = 0
        for f in span:
            live = [v for surf in part.surfaces for v in part_vertices(surf, f)]
            acc = 0.0
            for (bp, _bn), (lp, _ln) in zip(base, live):
                acc += ((bp[0] - lp[0]) ** 2 + (bp[1] - lp[1]) ** 2
                        + (bp[2] - lp[2]) ** 2)
            rms = math.sqrt(acc / n)
            total += rms
            count += 1
            worst = max(worst, rms)
        out.append((owner, n, total / max(count, 1), worst))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("playerdir")
    ap.add_argument("out", nargs="?")
    ap.add_argument("--tier", default="tier1")
    ap.add_argument("--ref-frame", type=int, default=-1,
                    help="freeze the meshes at this md3 frame (default: idle)")
    ap.add_argument("--report", action="store_true",
                    help="print the rigid-bind residual instead of converting")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    char = Character(args.playerdir, args.tier)
    ref = args.ref_frame if args.ref_frame >= 0 else char.reference_frame()

    if args.report:
        name = os.path.basename(args.playerdir.rstrip("/"))
        print("%-14s %d frames, reference frame %d" % (name, char.frames, ref))
        idle = None
        if len(char.anims) > IDLE_ROW:
            first, count = char.anims[IDLE_ROW][0], char.anims[IDLE_ROW][1]
            idle = range(first, min(first + count, char.frames))
        for label, span in (("all", None), ("idle", idle)):
            if span is None and label != "all":
                continue
            for owner, n, mean, worst in report(char, ref, span):
                print("    %-4s %-6s %5d verts   mean RMS %6.2f   worst %6.2f units"
                      % (label, owner, n, mean, worst))
        return 0

    if not args.out:
        ap.error("an output path is required unless --report is given")
    model = build(char, ref)
    size = iqm.save(model, args.out)
    if not args.quiet:
        verts = sum(len(m.positions) for m in model.meshes)
        print("%s: %d joints, %d meshes, %d verts, %d frames, %d bytes"
              % (os.path.basename(args.out), len(model.joints),
                 len(model.meshes), verts, len(model.frames), size))
    return 0


if __name__ == "__main__":
    sys.exit(main())
