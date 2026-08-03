#!/usr/bin/env python3
"""Read and write id Tech 3 MD3 models.

Enough of the format to do two jobs this branch needs:

  * dump a head's UV islands, so a repaint can be checked against the layout it
    is actually painted onto rather than against how the sheet looks flat;
  * build a small mesh from scratch and merge it into an existing head as an
    extra surface, which is how the masters get headgear without a modelling
    tool in the loop.

The format is little-endian throughout, vertices are 1/64 world unit shorts and
normals are a packed lat/lng pair. Frames carry their own bounds, and a surface
carries one set of XYZ per frame - so a mesh merged into an animated head has to
repeat its vertices for every frame, which is what add_surface does.
"""
import struct

MD3_IDENT = b"IDP3"
MD3_VERSION = 15
MD3_XYZ_SCALE = 1.0 / 64.0
NAME = 64


def _cstr(b):
    return b.split(b"\0", 1)[0].decode("latin-1")


def _pad(s, n=NAME):
    b = s.encode("latin-1")[: n - 1]
    return b + b"\0" * (n - len(b))


class Surface:
    def __init__(self):
        self.name = ""
        self.flags = 0
        self.shaders = []          # list of (name, index)
        self.triangles = []        # list of (a, b, c)
        self.st = []               # list of (s, t)
        self.frames = []           # per frame: list of (x, y, z, normal)

    @property
    def numVerts(self):
        return len(self.st)


class MD3:
    def __init__(self):
        self.name = ""
        self.flags = 0
        self.frames = []           # (mins, maxs, origin, radius, name)
        self.tags = []             # per frame: list of (name, origin, axis)
        self.surfaces = []


def load(path):
    with open(path, "rb") as fh:
        data = fh.read()
    return loads(data)


def loads(data):
    ident, version = struct.unpack_from("<4si", data, 0)
    if ident != MD3_IDENT:
        raise ValueError("not an MD3: %r" % ident)
    if version != MD3_VERSION:
        raise ValueError("MD3 version %d not supported" % version)
    m = MD3()
    m.name = _cstr(data[8:8 + NAME])
    (m.flags, numFrames, numTags, numSurfaces, _numSkins,
     ofsFrames, ofsTags, ofsSurfaces, _ofsEof) = struct.unpack_from("<9i", data, 8 + NAME)

    off = ofsFrames
    for _ in range(numFrames):
        vals = struct.unpack_from("<10f", data, off)
        nm = _cstr(data[off + 40:off + 40 + 16])
        m.frames.append((vals[0:3], vals[3:6], vals[6:9], vals[9], nm))
        off += 56

    off = ofsTags
    for _ in range(numFrames):
        row = []
        for _t in range(numTags):
            nm = _cstr(data[off:off + NAME])
            f = struct.unpack_from("<12f", data, off + NAME)
            row.append((nm, f[0:3], (f[3:6], f[6:9], f[9:12])))
            off += NAME + 48
        m.tags.append(row)

    off = ofsSurfaces
    for _ in range(numSurfaces):
        base = off
        sident = data[off:off + 4]
        if sident != MD3_IDENT:
            raise ValueError("bad surface ident %r" % sident)
        s = Surface()
        s.name = _cstr(data[off + 4:off + 4 + NAME])
        (s.flags, sFrames, sShaders, sVerts, sTris,
         ofsTris, ofsShaders, ofsSt, ofsXyz, ofsEnd) = struct.unpack_from(
            "<10i", data, off + 4 + NAME)

        p = base + ofsShaders
        for _i in range(sShaders):
            s.shaders.append((_cstr(data[p:p + NAME]),
                              struct.unpack_from("<i", data, p + NAME)[0]))
            p += NAME + 4

        p = base + ofsTris
        for _i in range(sTris):
            s.triangles.append(struct.unpack_from("<3i", data, p))
            p += 12

        p = base + ofsSt
        for _i in range(sVerts):
            s.st.append(struct.unpack_from("<2f", data, p))
            p += 8

        p = base + ofsXyz
        for _f in range(sFrames):
            verts = []
            for _i in range(sVerts):
                x, y, z, n = struct.unpack_from("<4h", data, p)
                verts.append((x, y, z, n))
                p += 8
            s.frames.append(verts)

        m.surfaces.append(s)
        off = base + ofsEnd
    return m


def dumps(m):
    numFrames = len(m.frames)
    numTags = len(m.tags[0]) if m.tags else 0

    frames = b""
    for mins, maxs, origin, radius, nm in m.frames:
        frames += struct.pack("<10f", *mins, *maxs, *origin, radius)
        frames += _pad(nm, 16)

    tags = b""
    for row in m.tags:
        for nm, origin, axis in row:
            tags += _pad(nm) + struct.pack("<12f", *origin, *axis[0], *axis[1], *axis[2])

    surfaces = b""
    for s in m.surfaces:
        shaders = b"".join(_pad(n) + struct.pack("<i", i) for n, i in s.shaders)
        tris = b"".join(struct.pack("<3i", *t) for t in s.triangles)
        st = b"".join(struct.pack("<2f", *c) for c in s.st)
        xyz = b""
        for verts in s.frames:
            xyz += b"".join(struct.pack("<4h", *v) for v in verts)

        hdr = 4 + NAME + 40
        ofsShaders = hdr
        ofsTris = ofsShaders + len(shaders)
        ofsSt = ofsTris + len(tris)
        ofsXyz = ofsSt + len(st)
        ofsEnd = ofsXyz + len(xyz)
        surfaces += (MD3_IDENT + _pad(s.name)
                     + struct.pack("<10i", s.flags, len(s.frames), len(s.shaders),
                                   len(s.st), len(s.triangles),
                                   ofsTris, ofsShaders, ofsSt, ofsXyz, ofsEnd)
                     + shaders + tris + st + xyz)

    hdr = 8 + NAME + 36
    ofsFrames = hdr
    ofsTags = ofsFrames + len(frames)
    ofsSurfaces = ofsTags + len(tags)
    ofsEof = ofsSurfaces + len(surfaces)
    head = (MD3_IDENT + struct.pack("<i", MD3_VERSION) + _pad(m.name)
            + struct.pack("<9i", m.flags, numFrames, numTags, len(m.surfaces), 0,
                          ofsFrames, ofsTags, ofsSurfaces, ofsEof))
    return head + frames + tags + surfaces


def save(m, path):
    with open(path, "wb") as fh:
        fh.write(dumps(m))


# --------------------------------------------------------------- normals

def pack_normal(x, y, z):
    """id's lat/lng byte pair. A zero-length normal packs as straight up.

    The field is written with the signed short the rest of the vertex uses, so
    the top bit of the latitude byte has to be carried into the sign rather
    than overflowing the pack.
    """
    import math
    n = math.sqrt(x * x + y * y + z * z)
    if n < 1e-9:
        return 0
    x, y, z = x / n, y / n, z / n
    lat = math.atan2(y, x) * 255.0 / (2.0 * math.pi)
    lng = math.acos(max(-1.0, min(1.0, z))) * 255.0 / (2.0 * math.pi)
    v = ((int(lat) & 255) << 8) | (int(lng) & 255)
    return v - 65536 if v > 32767 else v


def add_surface(m, surf):
    """Append a surface, repeating its single frame to match the model.

    A head merged onto an animated model must carry one XYZ set per frame or
    the renderer reads past the end of the array. Headgear does not deform, so
    every frame gets the same vertices.
    """
    if len(surf.frames) != len(m.frames):
        one = surf.frames[0]
        surf.frames = [list(one) for _ in range(len(m.frames))]
    m.surfaces.append(surf)
    return m
