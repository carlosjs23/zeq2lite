#!/usr/bin/env python3
"""Generate the screen-space aura ring mesh as an IQM model.

The aura is a flat annulus of quads that the vertex shader reshapes entirely:
it reads each vertex's position relative to the ring centre out of the vertex
colour channel, then rebuilds the geometry against the player's screen-space
bounding box. Nothing about the authored positions survives to the screen, but
they are still emitted as a real ring so the model has sane bounds and so the
mesh is recognisable in a viewer.

Why generated rather than authored in a modelling package:

  - The per-vertex payload is exact. R and G carry the unit-circle position
    remapped to 0..1 (so the centre is 0.5, 0.5) and B flags inner vs outer.
    Those are data, not art, and round-tripping them through an exporter's
    colour handling is a good way to lose the low bits.
  - IQM's vertex path in this engine is skinning-only, so the mesh needs one
    identity root joint with every weight at 255 and exactly one frame. A mesh
    without those renders invisible - see tests/suites/test_iqm.c, which pins
    both constraints.

Each quad owns four unshared vertices. That is deliberate: the technique gives
every quad the full 0..1 texture range, and the U direction is mirrored on one
side of the aura so the texture always flows toward the tip. Shared vertices
cannot hold two different UVs, so sharing them would fuse the seam.

usage:
    make_aura_mesh.py <out.iqm> [--segments N] [--inner R] [--outer R]
"""

import argparse
import math
import struct
import sys

IQM_MAGIC = b"INTERQUAKEMODEL\0"
IQM_VERSION = 2

# vertexarray types
IQM_POSITION, IQM_TEXCOORD, IQM_NORMAL = 0, 1, 2
IQM_BLENDINDEXES, IQM_BLENDWEIGHTS, IQM_COLOR = 4, 5, 6
# vertexarray formats
IQM_UBYTE, IQM_FLOAT = 1, 7

HEADER_SIZE = 16 + 27 * 4          # magic + 27 uint32
VERTEXARRAY_SIZE = 5 * 4
MESH_SIZE = 6 * 4
TRIANGLE_SIZE = 3 * 4
JOINT_SIZE = 2 * 4 + 10 * 4
POSE_SIZE = 2 * 4 + 20 * 4


def build_geometry(segments, inner_r, outer_r):
    """Four unshared vertices per quad, laid out inner-first."""
    positions, texcoords, normals, colors = [], [], [], []
    triangles = []

    def encode(rel):
        """Unit-circle coordinate (-1..1) to a colour byte, centre at 0.5."""
        return max(0, min(255, int(round((rel + 1.0) * 0.5 * 255.0))))

    for i in range(segments):
        a0 = (i / segments) * 2.0 * math.pi
        a1 = ((i + 1) / segments) * 2.0 * math.pi

        base = len(positions)

        # U advances around the ring rather than restarting per quad, so the
        # spike strip wraps the aura once and the wavelength parameter counts
        # wraps. Giving every quad the full 0..1 range - which is how the
        # original write-up describes it - multiplies the strip's own spike
        # count by the segment count instead: 32 quads of an 8-spike strip is
        # 256 spikes before wavelength is even applied, which reads as fur.
        # It also means every quad shows an identical spike, so the strip's
        # height variation never appears.
        #
        # v runs inner(0) to outer(1), so the texture's vertical axis is the
        # spike's length, which is what the amplitude parameter scales.
        u0 = i / segments
        u1 = (i + 1) / segments
        for angle, radius, outer, u, v in (
            (a0, inner_r, 0, u0, 0.0),
            (a1, inner_r, 0, u1, 0.0),
            (a1, outer_r, 1, u1, 1.0),
            (a0, outer_r, 1, u0, 1.0),
        ):
            cx, cy = math.cos(angle), math.sin(angle)
            positions.append((cx * radius, cy * radius, 0.0))
            texcoords.append((u, v))
            normals.append((0.0, 0.0, 1.0))
            colors.append((encode(cx), encode(cy), 255 if outer else 0, 255))

        triangles.append((base + 0, base + 1, base + 2))
        triangles.append((base + 0, base + 2, base + 3))

    return positions, texcoords, normals, colors, triangles


def build_iqm(segments, inner_r, outer_r):
    positions, texcoords, normals, colors, triangles = build_geometry(
        segments, inner_r, outer_r)
    nverts, ntris = len(positions), len(triangles)

    # --- text blob: mesh name and material -------------------------------
    text = b"\0"                       # index 0 is the empty string
    ofs_name = len(text)
    text += b"aura\0"
    ofs_material = len(text)
    text += b"aura\0"
    ofs_joint_name = len(text)
    text += b"root\0"
    while len(text) % 4:
        text += b"\0"

    # --- vertex data ------------------------------------------------------
    pos_blob = b"".join(struct.pack("<3f", *p) for p in positions)
    tc_blob = b"".join(struct.pack("<2f", *t) for t in texcoords)
    nrm_blob = b"".join(struct.pack("<3f", *n) for n in normals)
    bidx_blob = b"".join(struct.pack("<4B", 0, 0, 0, 0) for _ in positions)
    # All weight on the single root joint. 255 is "full" - the vertex path
    # divides the accumulated matrix by 255.
    bwgt_blob = b"".join(struct.pack("<4B", 255, 0, 0, 0) for _ in positions)
    col_blob = b"".join(struct.pack("<4B", *c) for c in colors)

    tri_blob = b"".join(struct.pack("<3I", *t) for t in triangles)

    # Identity joint: no parent, no translation, identity quaternion, unit
    # scale. The loader builds poseMat = pose * inverse(baseFrame), so an
    # identity joint under an identity pose yields the identity matrix and
    # authored positions pass through untouched.
    joint_blob = struct.pack("<Ii3f4f3f", ofs_joint_name, -1,
                             0.0, 0.0, 0.0,
                             0.0, 0.0, 0.0, 1.0,
                             1.0, 1.0, 1.0)

    # mask 0 means every channel comes from channeloffset alone, so this frame
    # consumes no framedata at all and num_framechannels stays 0.
    channeloffset = [0.0, 0.0, 0.0,  0.0, 0.0, 0.0, 1.0,  1.0, 1.0, 1.0]
    channelscale = [0.0] * 10
    pose_blob = struct.pack("<iI10f10f", -1, 0, *channeloffset, *channelscale)

    # iqmBounds_t is bbmin[3], bbmax[3], xyradius, radius - eight floats, not
    # six. The loader range-checks ofs_bounds against sizeof(iqmBounds_t), so a
    # short record is rejected outright with no explanation beyond "couldn't
    # load iqm file".
    xs = [p[0] for p in positions]
    ys = [p[1] for p in positions]
    xyradius = max(math.hypot(p[0], p[1]) for p in positions)
    radius = max(math.sqrt(p[0] ** 2 + p[1] ** 2 + p[2] ** 2) for p in positions)
    bounds_blob = struct.pack("<8f", min(xs), min(ys), 0.0,
                                     max(xs), max(ys), 0.0,
                                     xyradius, radius)

    # --- lay the file out -------------------------------------------------
    off = HEADER_SIZE
    ofs_text = off;            off += len(text)
    ofs_meshes = off;          off += MESH_SIZE
    ofs_vertexarrays = off;    off += 6 * VERTEXARRAY_SIZE
    ofs_position = off;        off += len(pos_blob)
    ofs_texcoord = off;        off += len(tc_blob)
    ofs_normal = off;          off += len(nrm_blob)
    ofs_blendindexes = off;    off += len(bidx_blob)
    ofs_blendweights = off;    off += len(bwgt_blob)
    ofs_color = off;           off += len(col_blob)
    ofs_triangles = off;       off += len(tri_blob)
    ofs_joints = off;          off += JOINT_SIZE
    ofs_poses = off;           off += POSE_SIZE
    ofs_bounds = off;          off += len(bounds_blob)
    filesize = off

    mesh_blob = struct.pack("<6I", ofs_name, ofs_material, 0, nverts, 0, ntris)

    va = [
        (IQM_POSITION,     0, IQM_FLOAT, 3, ofs_position),
        (IQM_TEXCOORD,     0, IQM_FLOAT, 2, ofs_texcoord),
        (IQM_NORMAL,       0, IQM_FLOAT, 3, ofs_normal),
        (IQM_BLENDINDEXES, 0, IQM_UBYTE, 4, ofs_blendindexes),
        (IQM_BLENDWEIGHTS, 0, IQM_UBYTE, 4, ofs_blendweights),
        (IQM_COLOR,        0, IQM_UBYTE, 4, ofs_color),
    ]
    va_blob = b"".join(struct.pack("<5I", *entry) for entry in va)

    header = IQM_MAGIC + struct.pack(
        "<27I",
        IQM_VERSION, filesize, 0,
        len(text), ofs_text,
        1, ofs_meshes,
        len(va), nverts, ofs_vertexarrays,
        ntris, ofs_triangles, 0,          # ofs_adjacency unused
        1, ofs_joints,
        1, ofs_poses,
        0, 0,                             # no anims
        1, 0, 0, ofs_bounds,              # 1 frame, 0 framechannels
        0, 0,                             # no comment
        0, 0,                             # no extensions
    )

    blob = (header + text + mesh_blob + va_blob + pos_blob + tc_blob +
            nrm_blob + bidx_blob + bwgt_blob + col_blob + tri_blob +
            joint_blob + pose_blob + bounds_blob)

    assert len(blob) == filesize, (len(blob), filesize)
    return blob, nverts, ntris


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("output")
    ap.add_argument("--segments", type=int, default=32,
                    help="quads around the ring (default: 32)")
    ap.add_argument("--inner", type=float, default=0.5,
                    help="inner radius in model units (default: 0.5)")
    ap.add_argument("--outer", type=float, default=1.0,
                    help="outer radius in model units (default: 1.0)")
    args = ap.parse_args()

    if args.segments < 3:
        sys.exit("--segments must be at least 3")
    if not 0.0 < args.inner < args.outer:
        sys.exit("need 0 < --inner < --outer")

    blob, nverts, ntris = build_iqm(args.segments, args.inner, args.outer)
    with open(args.output, "wb") as fh:
        fh.write(blob)

    print("%s: %d segments, %d vertices, %d triangles, %d bytes"
          % (args.output, args.segments, nverts, ntris, len(blob)))


if __name__ == "__main__":
    main()
