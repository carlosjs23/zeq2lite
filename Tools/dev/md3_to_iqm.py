#!/usr/bin/env python3
"""Build a skeletal IQM character out of a ZEQ2 three-part md3 set.

A ZEQ2 character is lower.md3, upper.md3 and head.md3 joined per frame on
tag_torso and tag_head, with one .skin file over all three. This produces the
same character as one IQM entity on one skeleton, so that gear can hang off a
bone instead of being baked into a head mesh, and so that a game module can be
handed a bone to drive.

There are two binds here and they are not close in quality.

**Rigid** (the default, no --bones). lower, upper and head each become one
bone, bound rigidly, with their vertices frozen at one reference frame; the
per-frame tag chain becomes the bones' animation. Everything *inside* a part is
lost - striding legs, swinging arms. For a master standing at his mark that is
the whole animation and the loss is nil; for a fighter mid-combo it is most of
it.

**Decomposed** (--bones N). The bones are solved out of the vertex
trajectories by `ssdr.py` rather than taken from the tag chain: every vertex's
path through every frame is the fit's ground truth, clusters of mutually rigid
vertices become the bones, and each vertex gets up to four blend weights. This
recovers deformation inside a part, which is the entire difference between a
converted master and a converted fighter.

Both binds share what the engine forces: every md3 tag becomes a bone of its
own carrying that tag's model-space transform, so trap_R_LerpTag keeps working
by name - R_IQMLerpTag matches joints by name and this writer binds every joint
at the identity, which is the condition under which the matrix it returns is
the joint's transform rather than a skinning matrix.

usage:
    md3_to_iqm.py <playerdir> <out.iqm> [--tier tier1] [--bones 24]
    md3_to_iqm.py <playerdir> --report [--tier tier1] [--bones 24]
    md3_to_iqm.py <playerdir> --sweep 8,16,24,32 [--tier tier1]
"""

import argparse
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

# Row ranges of animNumber_t (Game/Game/bg_public.h), grouped into the classes
# the error table is reported by. Half-open, in enum order. Positional like the
# file itself - a character whose animation.cfg is shorter simply has fewer.
ANIM_CLASSES = (
    ("death", 0, 3),
    ("stun", 3, 5),
    ("idle", 5, 8),
    ("walk", 8, 9),
    ("run", 9, 10),
    ("swim", 10, 12),
    ("jump", 12, 18),
    ("dash", 18, 22),
    ("fly", 22, 28),
    ("kicharge", 28, 33),
    ("defense", 33, 37),
    ("melee", 37, 66),
    ("knockback", 66, 70),
    ("kiattack", 70, 94),
)


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

    def spans(self):
        """(class name, frame range) for every animation.cfg row that exists."""
        out = []
        for name, first, last in ANIM_CLASSES:
            span = []
            for row in self.anims[first:last]:
                start, count = row[0], row[1]
                span.extend(range(max(start, 0), min(start + count, self.frames)))
            if span:
                out.append((name, sorted(set(span))))
        return out

    # --- the decomposition's input ----------------------------------------
    #
    # Per part, in the part's own md3 space, because that is the unit the game
    # animates: CG_PlayerAnimation hands legs, torso and head their own frame
    # numbers, so a fighter can run with his legs while his torso throws a
    # punch. One skeleton over the whole character has one frame index and
    # cannot do that. Solving each part separately keeps the split intact, and
    # the tag chain that joins them is untouched - a converted part is a drop-in
    # for its md3, tags, skins, damage states and all.

    def trajectories(self, owner):
        """One part's vertices, every frame: (verts [F, n, 3], layout)."""
        import numpy as np

        part = getattr(self, owner)
        frames = self.frames
        chunks = []
        layout = []
        offset = 0
        for surf in part.surfaces:
            arr = np.array([[(v[0], v[1], v[2]) for v in surf.frames[
                min(f, len(surf.frames) - 1)]] for f in range(frames)],
                dtype=np.float64) * md3.MD3_XYZ_SCALE
            layout.append((surf, offset, arr.shape[1]))
            offset += arr.shape[1]
            chunks.append(arr)
        if not chunks:
            raise ValueError("%s has no surfaces" % owner)
        return np.concatenate(chunks, axis=1), layout

    def rest_normals(self, owner, ref):
        """One part's normals at the reference frame, in trajectory order."""
        out = []
        for surf in getattr(self, owner).surfaces:
            for _p, n in part_vertices(surf, ref):
                out.append(n)
        return out


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

    # --- meshes, in their own bone's space --------------------------------
    #
    # Each part's md3 vertices go in untransformed, because a part's md3 space
    # *is* its bone's space: the assembly places upper.md3's origin at
    # tag_torso and head.md3's origin at tag_head, so a bone whose pose is that
    # tag maps the part's own coordinates into the model exactly as
    # CG_PositionRotatedEntityOnTag did.
    #
    # That choice is what makes a bone rotation pivot where a person's joint
    # is. Baking the vertices into model space instead would work identically
    # at rest and rotate the head about the character's feet the moment
    # anything drove the bone.
    parts = (("lower", char.lower, j_lower, ref),
             ("upper", char.upper, j_upper, ref),
             ("head", char.head, j_head, 0))
    for _owner, part, bone, frame in parts:
        for surf in part.surfaces:
            target = j_gear if surf.name.lower() == GEAR_SURFACE else bone
            mesh = iqm.Mesh(surf.name, surf.shaders[0][0] if surf.shaders else "")
            for (p, n), uv in zip(part_vertices(surf, frame), surf.st):
                mesh.add_vertex(p, uv, n, (target, 0, 0, 0), (255, 0, 0, 0))
            mesh.triangles = [tuple(t) for t in surf.triangles]
            model.meshes.append(mesh)

    # --- the pose track ---------------------------------------------------
    for f in range(char.frames):
        torso = char.torso(f)
        head = char.head_at(f)
        g = [None] * len(model.joints)
        g[j_root] = iqm.IDENTITY
        g[j_lower] = iqm.IDENTITY
        g[j_upper] = torso
        g[j_head] = head
        g[j_gear] = head
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


def solve_part(char, owner, ref, bones, iterations, log=None):
    """Decompose one part. Returns (verts, layout, solution)."""
    import ssdr

    verts, layout = char.trajectories(owner)
    sol = ssdr.solve(verts, ref, bones, iterations=iterations, log=log)
    return verts, layout, sol


def build_solved(char, owner, ref, layout, sol):
    """Assemble one part's iqm.Model from its solved rig.

    The result is a drop-in for that part's md3: same surfaces under the same
    names, same tags at the same per-frame transforms, same frame numbering.
    What changed is that the vertices now move, because the solved bones carry
    the deformation the md3 stored per vertex.

    Poses are the bones' own transforms. The mesh is authored in the part's own
    space at the reference frame and every joint binds at the identity, so a
    bone's global pose *is* the matrix the skinner multiplies its vertices by -
    which is exactly what the solve produced.
    """
    import numpy as np
    import ssdr

    part = getattr(char, owner)
    model = iqm.Model()
    model.anim_name = "md3"
    model.framerate = 20.0

    root, parent = ssdr.hierarchy(sol)
    names = ssdr.region_names(sol)
    # Joints have to be added parent-first, so walk the tree from the root.
    order = [root]
    for b in order:
        order.extend(i for i, p in enumerate(parent) if p == b)
    joint_of = {}
    for b in order:
        joint_of[b] = model.add_joint(
            names[b], -1 if parent[b] < 0 else joint_of[parent[b]])

    # Tag joints, parented to whichever solved bone sits nearest the tag at the
    # reference frame. The parent is cosmetic - the pose track states the tag's
    # transform in the part's space either way - but a tag hanging off the bone
    # it rides makes the tree read as a skeleton.
    weight_sum = sol.weights.sum(0)
    weight_sum[weight_sum <= 0] = 1.0
    centres = (sol.weights.T @ sol.rest) / weight_sum[:, None]
    tags = []
    if part.tags:
        for name, origin, _axis in part.tags[min(ref, len(part.tags) - 1)]:
            near = int(np.argmin(np.linalg.norm(centres - np.array(origin),
                                                axis=1)))
            tags.append((model.add_joint(name, joint_of[near]), name))

    # --- meshes, in the part's own space at the reference frame -----------
    normals = char.rest_normals(owner, ref)
    for surf, start, count in layout:
        mesh = iqm.Mesh(surf.name, surf.shaders[0][0] if surf.shaders else "")
        for k in range(count):
            v = start + k
            inf = sol.influences(v)
            bones = [joint_of[b] for b, _w in inf]
            raw = [int(round(w * ssdr.WEIGHT_SCALE)) for _b, w in inf]
            raw[0] += ssdr.WEIGHT_SCALE - sum(raw)
            bones += [0] * (4 - len(bones))
            raw += [0] * (4 - len(raw))
            mesh.add_vertex(tuple(sol.rest[v]), surf.st[k], normals[v],
                            tuple(bones[:4]), tuple(raw[:4]))
        mesh.triangles = [tuple(t) for t in surf.triangles]
        model.meshes.append(mesh)

    # --- the pose track ---------------------------------------------------
    for f in range(char.frames):
        g = [None] * len(model.joints)
        for b in range(sol.bones):
            m = [0.0] * 12
            for r in range(3):
                m[4 * r:4 * r + 3] = [float(x) for x in sol.rot[b, f, r]]
                m[4 * r + 3] = float(sol.trans[b, f, r])
            g[joint_of[b]] = m
        live = tag_map(part, f)
        for index, name in tags:
            g[index] = live.get(name, iqm.IDENTITY)
        # Poses are stated relative to the parent; the bind is the identity, so
        # a joint's own transform goes in unchanged at the root.
        frame = []
        for i, joint in enumerate(model.joints):
            if joint.parent < 0:
                frame.append(g[i])
            else:
                frame.append(iqm.mat_mul(iqm.mat_invert_rigid(g[joint.parent]),
                                         g[i]))
        model.frames.append(frame)
    return model


def rigid_reconstruction(verts, ref):
    """Where the rigid bind puts a part's vertices: frozen at the reference.

    A part's bone reproduces the part's placement exactly - the pose is the md3
    tag chain - so in the part's own space the rigid bind is the rest pose held
    still, and the residual is the whole of the deformation inside the part.
    That is the same quantity the published 9-15 unit table measured.
    """
    import numpy as np

    return np.repeat(verts[ref][None], verts.shape[0], axis=0)


# The parts a character is made of, in registration order, and the bone count
# each is solved at by default. They differ because the parts do: the legs are
# a few rigid segments and saturate by twelve bones, the torso carries two arms
# and keeps improving past twenty-four, and the head barely deforms at all -
# and handing the head more bones than its motion needs makes the fit *worse*,
# because the clustering has more ways to land in a poor local minimum. See
# --sweep, and Tools/dev/README.md for the table these came from.
PARTS = ("lower", "upper", "head")
DEFAULT_BONES = {"lower": 16, "upper": 24, "head": 12}


def parse_bones(spec):
    """"24" for every part, or "lower=16,upper=24,head=12" for each."""
    out = dict(DEFAULT_BONES)
    if not spec:
        return out
    if "=" not in spec:
        return {part: int(spec) for part in PARTS}
    for item in spec.split(","):
        key, _sep, value = item.partition("=")
        key = key.strip()
        if key not in out:
            raise ValueError("unknown part %r" % key)
        out[key] = int(value)
    return out


def error_table(char, ref, verts, rigid, decomposed=None):
    """Mean per-frame RMS, per animation class, for one part and both binds."""
    import numpy as np
    import ssdr

    rows = []
    spans = [("all", list(range(char.frames)))] + char.spans()
    for name, span in spans:
        idx = np.array(span, dtype=int)
        a = float(ssdr.frame_rms(verts[idx], rigid[idx]).mean())
        b = (float(ssdr.frame_rms(verts[idx], decomposed[idx]).mean())
             if decomposed is not None else None)
        rows.append((name, len(idx), a, b))
    return rows


def convert_parts(char, ref, bones, iterations, outdir, log=None,
                  quiet=False):
    """Solve and write lower.iqm, upper.iqm and head.iqm. Returns the errors."""
    import ssdr

    summary = []
    for owner in PARTS:
        if log:
            log("%s:" % owner)
        verts, layout, sol = solve_part(char, owner, ref, bones[owner],
                                        iterations, log)
        got = ssdr.reconstruct(verts.shape, sol.rest, sol.rot, sol.trans,
                               sol.weights)
        rigid = rigid_reconstruction(verts, ref)
        model = build_solved(char, owner, ref, layout, sol)
        path = os.path.join(outdir, owner + ".iqm")
        size = iqm.save(model, path)
        rows = error_table(char, ref, verts, rigid, got)
        summary.append((owner, rows, verts.shape[1], len(model.joints), size))
        if not quiet:
            print("    %-6s %d joints, %d verts, %d bytes, RMS %.2f -> %.2f"
                  % (owner, len(model.joints), verts.shape[1], size,
                     rows[0][2], rows[0][3]), flush=True)
    return summary


def whole_character(summary):
    """Fold the three parts' tables into one, weighted by vertex count.

    RMS over a set of vertices is the root of their mean square, so parts
    combine as the root of the vertex-weighted mean of their squares - not as
    the mean of their RMS values, which would flatter a big part with a small
    residual.
    """
    import math

    classes = [row[0] for row in summary[0][1]]
    total = sum(count for _o, _r, count, _j, _s in summary)
    out = []
    for index, label in enumerate(classes):
        frames = summary[0][1][index][1]
        acc = [0.0, 0.0]
        for _owner, rows, count, _joints, _size in summary:
            for slot, value in enumerate(rows[index][2:4]):
                acc[slot] += count * value * value
        out.append((label, frames, math.sqrt(acc[0] / total),
                    math.sqrt(acc[1] / total)))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("playerdir")
    ap.add_argument("out", nargs="?")
    ap.add_argument("--tier", default="tier1")
    ap.add_argument("--ref-frame", type=int, default=-1,
                    help="author the meshes at this md3 frame (default: idle)")
    ap.add_argument("--bones", default="",
                    help="bones per part: one number, or lower=16,upper=24,"
                         "head=12 (default)")
    ap.add_argument("--iterations", type=int, default=6,
                    help="alternating least squares passes")
    ap.add_argument("--parts", action="store_true",
                    help="decompose each part and write lower/upper/head.iqm "
                         "beside the md3s")
    ap.add_argument("--report", action="store_true",
                    help="print the residual per animation class, both binds")
    ap.add_argument("--rigid-only", action="store_true",
                    help="report the rigid bind without running the solve")
    ap.add_argument("--sweep", default="",
                    help="comma-separated bone counts to report error against")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    char = Character(args.playerdir, args.tier)
    ref = args.ref_frame if args.ref_frame >= 0 else char.reference_frame()
    bones = parse_bones(args.bones)
    name = os.path.basename(args.playerdir.rstrip("/"))
    log = None if args.quiet else lambda s: print("    " + s, flush=True)

    if args.sweep:
        import ssdr
        print("%s  %d frames, reference frame %d"
              % (name, char.frames, ref))
        print("    %-6s %-6s %8s" % ("part", "bones", "RMS"))
        for owner in PARTS:
            verts, _layout = char.trajectories(owner)
            rigid = rigid_reconstruction(verts, ref)
            print("    %-6s %-6s %8.3f"
                  % (owner, "rigid", ssdr.frame_rms(verts, rigid).mean()))
            for count in [int(x) for x in args.sweep.split(",") if x.strip()]:
                sol = ssdr.solve(verts, ref, count, iterations=args.iterations)
                got = ssdr.reconstruct(verts.shape, sol.rest, sol.rot,
                                       sol.trans, sol.weights)
                print("    %-6s %-6d %8.3f"
                      % (owner, count, ssdr.frame_rms(verts, got).mean()),
                      flush=True)
        return 0

    if args.report:
        import ssdr
        print("%s  %d frames, reference frame %d, bones %s"
              % (name, char.frames, ref,
                 " ".join("%s=%d" % (p, bones[p]) for p in PARTS)))
        for owner in PARTS:
            verts, _layout = char.trajectories(owner)
            rigid = rigid_reconstruction(verts, ref)
            got = None
            if not args.rigid_only:
                sol = ssdr.solve(verts, ref, bones[owner],
                                 iterations=args.iterations)
                got = ssdr.reconstruct(verts.shape, sol.rest, sol.rot,
                                       sol.trans, sol.weights)
            print("  %s, %d verts" % (owner, verts.shape[1]))
            print("    %-10s %6s %8s %10s"
                  % ("class", "frames", "rigid", "decomposed"))
            for label, count, a, b in error_table(char, ref, verts, rigid, got):
                print("    %-10s %6d %8.2f %10s"
                      % (label, count, a, "-" if b is None else "%.2f" % b),
                      flush=True)
        return 0

    if args.parts:
        outdir = os.path.join(args.playerdir, args.tier)
        if not args.quiet:
            print("%s: solving %s" % (name, " ".join(
                "%s=%d" % (p, bones[p]) for p in PARTS)))
        summary = convert_parts(char, ref, bones, args.iterations, outdir,
                                log if not args.quiet else None, args.quiet)
        if not args.quiet:
            print("  whole character, %d verts"
                  % sum(c for _o, _r, c, _j, _s in summary))
            print("    %-10s %6s %8s %10s"
                  % ("class", "frames", "rigid", "decomposed"))
            for label, frames, a, b in whole_character(summary):
                print("    %-10s %6d %8.2f %10.2f" % (label, frames, a, b))
        return 0

    if not args.out:
        ap.error("an output path is required unless --report or --parts is given")
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
