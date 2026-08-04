#!/usr/bin/env python3
"""Write Inter-Quake Model files this engine's loader accepts.

make_aura_mesh.py already emitted an IQM, but only the one shape it needs: a
single mesh on a single identity joint with one frame. Skeletal characters need
the rest of the format - a joint hierarchy, several meshes, and a pose track -
so the writer moves here and the aura keeps its own copy of the geometry logic.

Three constraints come from Engine/renderer/tr_model_iqm.c rather than from the
IQM specification, and getting any of them wrong produces a model that loads and
renders as nothing:

  * num_poses must equal num_joints. The loader rejects the file otherwise.
  * The vertex path is skinning-only: every vertex needs a blend index and a
    weight, and the weights are divided by 255, so a vertex whose weights sum
    to zero collapses onto the origin.
  * A joint is only usable as a tag if its *bind* transform is the identity.
    R_IQMLerpTag hands back the skinning matrix, pose_global * inverse(bind_global),
    not the joint's world transform. Those are the same thing only when the bind
    is identity. So every joint this writer emits binds at the identity and the
    whole rest pose lives in frame 0 of the pose track - which also means mesh
    vertices are authored in model space and pass through untouched at rest.

Frame channels are quantised to 16 bits against a per-pose range, which is what
the format offers: channeloffset is the range's minimum and channelscale the
step. A channel that never varies is emitted with its mask bit clear so it costs
no frame data at all.
"""

import math
import struct

IQM_MAGIC = b"INTERQUAKEMODEL\0"
IQM_VERSION = 2

IQM_POSITION, IQM_TEXCOORD, IQM_NORMAL, IQM_TANGENT = 0, 1, 2, 3
IQM_BLENDINDEXES, IQM_BLENDWEIGHTS, IQM_COLOR = 4, 5, 6
IQM_UBYTE, IQM_FLOAT = 1, 7

HEADER_SIZE = 16 + 27 * 4
VERTEXARRAY_SIZE = 5 * 4
MESH_SIZE = 6 * 4
JOINT_SIZE = 2 * 4 + 10 * 4
POSE_SIZE = 2 * 4 + 20 * 4
ANIM_SIZE = 5 * 4
BOUNDS_SIZE = 8 * 4

MAX_JOINTS = 128        # IQM_MAX_JOINTS in Engine/renderer/iqm.h


# ------------------------------------------------------------------ matrices
#
# A transform here is a 3x4 row-major list of 12 floats, laid out exactly as
# JointToMatrix builds one: rows [0:4], [4:8], [8:12], translation in the
# fourth column. Keeping the tool's convention identical to the renderer's is
# what lets a pose be checked by hand against a jointMats dump.

IDENTITY = [1.0, 0.0, 0.0, 0.0,
            0.0, 1.0, 0.0, 0.0,
            0.0, 0.0, 1.0, 0.0]


def mat_mul(a, b):
    """a * b, both 3x4 with an implied [0 0 0 1] bottom row."""
    out = [0.0] * 12
    for r in range(3):
        for c in range(3):
            out[4 * r + c] = (a[4 * r + 0] * b[0 + c]
                              + a[4 * r + 1] * b[4 + c]
                              + a[4 * r + 2] * b[8 + c])
        out[4 * r + 3] = (a[4 * r + 0] * b[3]
                          + a[4 * r + 1] * b[7]
                          + a[4 * r + 2] * b[11]
                          + a[4 * r + 3])
    return out


def mat_invert_rigid(m):
    """Inverse of a rotation-plus-translation matrix: transpose, re-rotate t."""
    out = [m[0], m[4], m[8], 0.0,
           m[1], m[5], m[9], 0.0,
           m[2], m[6], m[10], 0.0]
    t = (m[3], m[7], m[11])
    for r in range(3):
        out[4 * r + 3] = -(out[4 * r + 0] * t[0]
                           + out[4 * r + 1] * t[1]
                           + out[4 * r + 2] * t[2])
    return out


def mat_from_axis_origin(axis, origin):
    """Build a 3x4 from an md3-style tag: axis[i] is the i'th basis vector.

    A tag's axis rows are the basis vectors expressed in the parent frame, so
    they form the columns of the matrix that maps tag space into parent space.
    """
    return [axis[0][0], axis[1][0], axis[2][0], origin[0],
            axis[0][1], axis[1][1], axis[2][1], origin[1],
            axis[0][2], axis[1][2], axis[2][2], origin[2]]


def mat_transform(m, v):
    return (m[0] * v[0] + m[1] * v[1] + m[2] * v[2] + m[3],
            m[4] * v[0] + m[5] * v[1] + m[6] * v[2] + m[7],
            m[8] * v[0] + m[9] * v[1] + m[10] * v[2] + m[11])


def mat_rotate(m, v):
    return (m[0] * v[0] + m[1] * v[1] + m[2] * v[2],
            m[4] * v[0] + m[5] * v[1] + m[6] * v[2],
            m[8] * v[0] + m[9] * v[1] + m[10] * v[2])


def mat_to_quat(m):
    """Rotation part to (x, y, z, w), the order JointToMatrix reads.

    Shepperd's method: pick the largest diagonal term so the divisor is never
    near zero. A near-zero divisor here shows up as a joint that flips at one
    frame of an otherwise smooth track, which reads as a broken export rather
    than as precision loss.
    """
    m00, m01, m02 = m[0], m[1], m[2]
    m10, m11, m12 = m[4], m[5], m[6]
    m20, m21, m22 = m[8], m[9], m[10]
    tr = m00 + m11 + m22
    if tr > 0.0:
        s = math.sqrt(tr + 1.0) * 2.0
        w = 0.25 * s
        x = (m21 - m12) / s
        y = (m02 - m20) / s
        z = (m10 - m01) / s
    elif m00 > m11 and m00 > m22:
        s = math.sqrt(1.0 + m00 - m11 - m22) * 2.0
        w = (m21 - m12) / s
        x = 0.25 * s
        y = (m01 + m10) / s
        z = (m02 + m20) / s
    elif m11 > m22:
        s = math.sqrt(1.0 + m11 - m00 - m22) * 2.0
        w = (m02 - m20) / s
        x = (m01 + m10) / s
        y = 0.25 * s
        z = (m12 + m21) / s
    else:
        s = math.sqrt(1.0 + m22 - m00 - m11) * 2.0
        w = (m10 - m01) / s
        x = (m02 + m20) / s
        y = (m12 + m21) / s
        z = 0.25 * s
    n = math.sqrt(x * x + y * y + z * z + w * w) or 1.0
    return (x / n, y / n, z / n, w / n)


def quat_align(q, ref):
    """Flip q onto the same hemisphere as ref.

    q and -q are the same rotation, and the pose track is quantised and then
    lerped as *matrices* by the renderer, so a sign flip between neighbouring
    frames is not a smoothness problem - but it doubles the quantisation range
    of four channels for nothing. Keeping the track on one hemisphere keeps the
    16-bit steps small.
    """
    if sum(a * b for a, b in zip(q, ref)) < 0.0:
        return (-q[0], -q[1], -q[2], -q[3])
    return q


def mat_to_channels(m, ref_quat=None):
    """The ten pose channels of a rigid transform: translate, rotate, scale."""
    q = mat_to_quat(m)
    if ref_quat is not None:
        q = quat_align(q, ref_quat)
    return [m[3], m[7], m[11], q[0], q[1], q[2], q[3], 1.0, 1.0, 1.0]


# ------------------------------------------------------------------- model

class Joint:
    def __init__(self, name, parent=-1):
        self.name = name
        self.parent = parent


class Mesh:
    def __init__(self, name, material):
        self.name = name
        self.material = material
        self.positions = []     # (x, y, z)
        self.texcoords = []     # (s, t)
        self.normals = []       # (x, y, z)
        self.blendindexes = []  # (i, i, i, i)
        self.blendweights = []  # (w, w, w, w), summing to 255
        self.triangles = []     # (a, b, c), indices local to this mesh

    def add_vertex(self, pos, uv, nrm, bones=(0, 0, 0, 0), weights=(255, 0, 0, 0)):
        self.positions.append(tuple(pos))
        self.texcoords.append(tuple(uv))
        self.normals.append(tuple(nrm))
        self.blendindexes.append(tuple(bones))
        self.blendweights.append(tuple(weights))
        return len(self.positions) - 1


class Model:
    """Joints, meshes and a pose track, ready to be serialised.

    frames is a list of frames; each frame is a list of 3x4 transforms, one per
    joint, expressed *relative to the joint's parent*. Frame 0 is the rest pose,
    because the joints themselves all bind at the identity.
    """

    def __init__(self):
        self.joints = []
        self.meshes = []
        self.frames = []
        self.framerate = 20.0
        self.anim_name = "idle"
        self.loop = True
        self._boxes = None

    def add_joint(self, name, parent=-1):
        if len(self.joints) >= MAX_JOINTS:
            raise ValueError("more than %d joints; IQM_MAX_JOINTS" % MAX_JOINTS)
        self.joints.append(Joint(name, parent))
        return len(self.joints) - 1

    def joint_index(self, name):
        for i, j in enumerate(self.joints):
            if j.name == name:
                return i
        raise KeyError(name)

    # --- bounds ---------------------------------------------------------
    #
    # The loader keeps per-frame bounds and R_CullIQM culls on them, so a model
    # whose bounds are stated once at the rest pose vanishes as soon as an
    # animation carries it outside them. Skinning every vertex through every
    # frame would be exact and is far too slow in Python for a 668-frame
    # character, so each bone's vertices are reduced once to a box and a radius
    # and only those eight corners are moved per frame. Transforming a box's
    # corners and re-bounding them always encloses the transformed contents, so
    # the result is conservative - which is the safe direction for a cull.

    def _global_mats(self, frame):
        out = []
        for i, joint in enumerate(self.joints):
            m = frame[i]
            out.append(m if joint.parent < 0 else mat_mul(out[joint.parent], m))
        return out

    def _bone_boxes(self):
        """Per bone: the eight corners of its vertices' box, and their radius.

        A vertex blended across several bones is charged to every bone it
        touches. That over-counts, which again only ever grows the bounds.
        """
        if getattr(self, "_boxes", None) is not None:
            return self._boxes
        acc = {}
        for mesh in self.meshes:
            for k, p in enumerate(mesh.positions):
                for b, w in zip(mesh.blendindexes[k], mesh.blendweights[k]):
                    if w <= 0:
                        continue
                    e = acc.get(b)
                    if e is None:
                        acc[b] = e = [list(p), list(p), 0.0]
                    for c in range(3):
                        if p[c] < e[0][c]:
                            e[0][c] = p[c]
                        if p[c] > e[1][c]:
                            e[1][c] = p[c]
                    e[2] = max(e[2], math.sqrt(p[0] ** 2 + p[1] ** 2 + p[2] ** 2))
        boxes = []
        for b, (lo, hi, rad) in acc.items():
            corners = [(lo[0] if i & 1 else hi[0],
                        lo[1] if i & 2 else hi[1],
                        lo[2] if i & 4 else hi[2]) for i in range(8)]
            boxes.append((b, corners, rad))
        self._boxes = boxes
        return boxes

    def frame_bounds(self, frame):
        mats = self._global_mats(frame)
        lo = [1e30] * 3
        hi = [-1e30] * 3
        xyr = r = 0.0
        for b, corners, rad in self._bone_boxes():
            m = mats[b]
            for p in corners:
                v = mat_transform(m, p)
                for c in range(3):
                    if v[c] < lo[c]:
                        lo[c] = v[c]
                    if v[c] > hi[c]:
                        hi[c] = v[c]
            # The bone's own displacement plus its reach, rather than the
            # corners' radii: a box corner is not the farthest point of a
            # sphere the bone can swing its vertices through.
            d = math.sqrt(m[3] ** 2 + m[7] ** 2 + m[11] ** 2)
            xyr = max(xyr, math.hypot(m[3], m[7]) + rad)
            r = max(r, d + rad)
        if lo[0] > hi[0]:
            lo = [0.0] * 3
            hi = [0.0] * 3
        return lo, hi, xyr, r


# ------------------------------------------------------------------- writer

def _text_blob(strings):
    """Pack strings into the text section, returning offsets for each."""
    blob = bytearray(b"\0")
    offsets = {}
    for s in strings:
        if s in offsets:
            continue
        offsets[s] = len(blob)
        blob += s.encode("latin-1") + b"\0"
    while len(blob) % 4:
        blob += b"\0"
    return bytes(blob), offsets


def dumps(model):
    if not model.joints:
        raise ValueError("an IQM this engine loads needs at least one joint")
    if not model.frames:
        raise ValueError("num_frames must be at least 1")
    njoints = len(model.joints)
    for f in model.frames:
        if len(f) != njoints:
            raise ValueError("frame has %d transforms, %d joints" % (len(f), njoints))

    names = [j.name for j in model.joints]
    for m in model.meshes:
        names += [m.name, m.material]
    names.append(model.anim_name)
    text, tofs = _text_blob(names)

    # --- pose channels, quantised per channel over the whole track --------
    tracks = []             # [joint][channel] -> list of values per frame
    for j in range(njoints):
        chans = []
        prev = None
        for f in model.frames:
            c = mat_to_channels(f[j], prev[3:7] if prev else None)
            prev = c
            chans.append(c)
        tracks.append(chans)

    pose_blob = b""
    framechannels = 0
    quant = []              # [joint] -> (mask, [ (offset, scale, chan) ])
    for j in range(njoints):
        mask = 0
        offsets = [0.0] * 10
        scales = [0.0] * 10
        active = []
        for c in range(10):
            vals = [tracks[j][f][c] for f in range(len(model.frames))]
            lo, hi = min(vals), max(vals)
            offsets[c] = lo
            if hi - lo > 1e-9:
                mask |= 1 << c
                scales[c] = (hi - lo) / 65535.0
                active.append(c)
        quant.append((mask, offsets, scales, active))
        framechannels += len(active)
        pose_blob += struct.pack("<iI10f10f", model.joints[j].parent, mask,
                                 *offsets, *scales)

    frame_blob = b""
    for f in range(len(model.frames)):
        row = []
        for j in range(njoints):
            mask, offsets, scales, active = quant[j]
            for c in active:
                v = (tracks[j][f][c] - offsets[c]) / scales[c]
                row.append(max(0, min(65535, int(round(v)))))
        frame_blob += struct.pack("<%dH" % len(row), *row)

    bounds_blob = b""
    for f in model.frames:
        lo, hi, xyr, r = model.frame_bounds(f)
        bounds_blob += struct.pack("<8f", *lo, *hi, xyr, r)

    joint_blob = b""
    for j in model.joints:
        joint_blob += struct.pack("<Ii3f4f3f", tofs[j.name], j.parent,
                                  0.0, 0.0, 0.0,
                                  0.0, 0.0, 0.0, 1.0,
                                  1.0, 1.0, 1.0)

    # --- vertex arrays, meshes concatenated into one flat buffer ----------
    pos_blob = tc_blob = nrm_blob = b""
    bidx_blob = bwgt_blob = tri_blob = b""
    mesh_recs = []
    first_vertex = first_triangle = 0
    for m in model.meshes:
        n = len(m.positions)
        pos_blob += b"".join(struct.pack("<3f", *p) for p in m.positions)
        tc_blob += b"".join(struct.pack("<2f", *t) for t in m.texcoords)
        nrm_blob += b"".join(struct.pack("<3f", *v) for v in m.normals)
        bidx_blob += b"".join(struct.pack("<4B", *b) for b in m.blendindexes)
        bwgt_blob += b"".join(struct.pack("<4B", *w) for w in m.blendweights)
        tri_blob += b"".join(struct.pack("<3I", a + first_vertex,
                                         b + first_vertex, c + first_vertex)
                             for a, b, c in m.triangles)
        mesh_recs.append((tofs[m.name], tofs[m.material], first_vertex, n,
                          first_triangle, len(m.triangles)))
        first_vertex += n
        first_triangle += len(m.triangles)
    nverts, ntris = first_vertex, first_triangle
    if nverts == 0:
        raise ValueError("no vertices")

    anim_blob = struct.pack("<3IfI", tofs[model.anim_name], 0,
                            len(model.frames), model.framerate,
                            1 if model.loop else 0)

    off = HEADER_SIZE
    ofs_text = off;         off += len(text)
    ofs_meshes = off;       off += MESH_SIZE * len(mesh_recs)
    ofs_vertexarrays = off; off += 5 * VERTEXARRAY_SIZE
    ofs_position = off;     off += len(pos_blob)
    ofs_texcoord = off;     off += len(tc_blob)
    ofs_normal = off;       off += len(nrm_blob)
    ofs_blendindexes = off; off += len(bidx_blob)
    ofs_blendweights = off; off += len(bwgt_blob)
    ofs_triangles = off;    off += len(tri_blob)
    ofs_joints = off;       off += JOINT_SIZE * njoints
    ofs_poses = off;        off += len(pose_blob)
    ofs_anims = off;        off += ANIM_SIZE
    ofs_frames = off;       off += len(frame_blob)
    ofs_bounds = off;       off += len(bounds_blob)
    filesize = off

    mesh_blob = b"".join(struct.pack("<6I", *r) for r in mesh_recs)
    va = [
        (IQM_POSITION,     0, IQM_FLOAT, 3, ofs_position),
        (IQM_TEXCOORD,     0, IQM_FLOAT, 2, ofs_texcoord),
        (IQM_NORMAL,       0, IQM_FLOAT, 3, ofs_normal),
        (IQM_BLENDINDEXES, 0, IQM_UBYTE, 4, ofs_blendindexes),
        (IQM_BLENDWEIGHTS, 0, IQM_UBYTE, 4, ofs_blendweights),
    ]
    va_blob = b"".join(struct.pack("<5I", *e) for e in va)

    header = IQM_MAGIC + struct.pack(
        "<27I",
        IQM_VERSION, filesize, 0,
        len(text), ofs_text,
        len(mesh_recs), ofs_meshes,
        len(va), nverts, ofs_vertexarrays,
        ntris, ofs_triangles, 0,
        njoints, ofs_joints,
        njoints, ofs_poses,
        1, ofs_anims,
        len(model.frames), framechannels, ofs_frames, ofs_bounds,
        0, 0,
        0, 0,
    )

    blob = (header + text + mesh_blob + va_blob + pos_blob + tc_blob
            + nrm_blob + bidx_blob + bwgt_blob + tri_blob + joint_blob
            + pose_blob + anim_blob + frame_blob + bounds_blob)
    assert len(blob) == filesize, (len(blob), filesize)
    return blob


def save(model, path):
    blob = dumps(model)
    with open(path, "wb") as fh:
        fh.write(blob)
    return len(blob)
