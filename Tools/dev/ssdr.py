#!/usr/bin/env python3
"""Solve a skeleton and skin weights out of vertex-animated geometry.

Smooth Skinning Decomposition with Rigid Bones (Le and Deng, SIGGRAPH Asia
2012), which is the only route from md3 to a skeletal format that does not
need source art: md3 stores every vertex in every frame, so the animation is
its own ground truth and the rig can be fitted to it rather than authored.

The unknowns are a bone count B worth of per-frame rigid transforms and a
weight per vertex per bone, and the objective is exactly what the renderer
computes - linear blend skinning of a single rest pose:

    v_hat(f, i) = sum_b  w[i, b] * ( R[b, f] p[i] + T[b, f] )

Alternating least squares: with the weights fixed each bone's transforms are a
weighted absolute-orientation problem with a closed form; with the transforms
fixed each vertex's weights are a small non-negative least squares. Both steps
lower the same residual, so the sequence is monotone up to the sparsity
projection.

Where this departs from the paper, and why:

  * Initialisation is k-means over whole vertex trajectories followed by a
    rigid-fit relabelling loop, rather than the paper's incremental
    bone-splitting. The relabelling loop is the part that matters - it is
    already the transform step with one-hot weights - and starting from the
    requested bone count instead of growing into it costs one parameter and
    saves the outer loop.
  * The sum-to-one constraint is a penalty row rather than a substitution, so
    both constraints go through one non-negative solve.
  * Weights are quantised to the renderer's 8 bits before the last transform
    update, so the reported residual is the residual of the model that ships
    and not of a float ideal the file cannot store.

Everything here is plain numpy. `--self-test` checks the pieces against
synthetic rigs with a known answer.
"""

import argparse
import sys

import numpy as np

# The renderer blends at most four bones per vertex (`tr_surface.c` reads
# blendIndexes[4*v + j] for j < 4 and stops at the first zero weight), and
# stores each weight as a byte summing to 255.
MAX_INFLUENCES = 4
WEIGHT_SCALE = 255


class Solution:
    """A solved rig: rest pose, per-bone per-frame transforms, weights."""

    def __init__(self, rest, rot, trans, weights):
        self.rest = rest            # [N, 3]   the pose the mesh is authored in
        self.rot = rot              # [B, F, 3, 3]
        self.trans = trans          # [B, F, 3]
        self.weights = weights      # [N, B], >= 0, rows sum to 1

    @property
    def bones(self):
        return self.rot.shape[0]

    @property
    def frames(self):
        return self.rot.shape[1]

    def influences(self, index):
        """(bone, weight) pairs for one vertex, largest first."""
        w = self.weights[index]
        order = np.argsort(-w)[:MAX_INFLUENCES]
        return [(int(b), float(w[b])) for b in order if w[b] > 0.0]


# --------------------------------------------------------------- primitives

def procrustes(src, dst, weight=None):
    """Rigid transforms taking src [n,3] onto dst [f,n,3], one per frame.

    Weighted Kabsch, solved for every frame at once. Returns (R [f,3,3],
    T [f,3]) minimising sum_i weight_i ||R src_i + T - dst_f_i||^2. The
    determinant fix-up is what keeps a reflection out when the point set is
    flat, which happens whenever a cluster lands on a single planar panel.
    """
    if weight is None:
        weight = np.ones(len(src))
    total = weight.sum()
    src_mid = (weight[:, None] * src).sum(0) / total
    dst_mid = np.einsum("n,fnj->fj", weight, dst) / total
    cov = np.einsum("n,ni,fnj->fij", weight, src - src_mid,
                    dst - dst_mid[:, None, :])
    u, _s, vt = np.linalg.svd(cov)
    rot = np.einsum("fji,fkj->fik", vt, u)
    flip = np.sign(np.linalg.det(rot))
    vt = vt.copy()
    vt[:, 2, :] *= flip[:, None]
    rot = np.einsum("fji,fkj->fik", vt, u)
    return rot, dst_mid - np.einsum("fij,j->fi", rot, src_mid)


def bone_positions(rot, trans, points):
    """Where each bone would put each point, per frame: [B, F, n, 3]."""
    return np.einsum("bfij,nj->bfni", rot, points) + trans[:, :, None, :]


def nnls(gram, rhs, tol=1e-10, maxiter=40):
    """min ||Ax - b|| s.t. x >= 0, given only gram = A'A and rhs = A'b.

    Lawson-Hanson active set. The systems here are at most eight unknowns, so
    the inner solve is a 8x8 and the loop bound is generous rather than tight.
    """
    n = len(rhs)
    x = np.zeros(n)
    free = np.zeros(n, dtype=bool)
    for _ in range(maxiter):
        grad = rhs - gram @ x
        grad[free] = -np.inf
        j = int(np.argmax(grad))
        if grad[j] <= tol:
            break
        free[j] = True
        for _ in range(maxiter):
            idx = np.flatnonzero(free)
            sub = gram[np.ix_(idx, idx)]
            try:
                s = np.linalg.solve(sub, rhs[idx])
            except np.linalg.LinAlgError:
                s = np.linalg.lstsq(sub, rhs[idx], rcond=None)[0]
            if (s > tol).all():
                x[:] = 0.0
                x[idx] = s
                break
            neg = s <= tol
            step = np.min(x[idx][neg] / (x[idx][neg] - s[neg] + 1e-300))
            x[idx] = x[idx] + step * (s - x[idx])
            free[idx[x[idx] <= tol]] = False
            x[~free] = 0.0
        else:
            break
    return x


# ------------------------------------------------------------ the ALS solve

def _kmeans(features, k, seed, iters=30):
    """k-means++ over rows of features. Labels only; centres are scratch."""
    rng = np.random.default_rng(seed)
    n = len(features)
    centres = np.empty((k, features.shape[1]))
    centres[0] = features[rng.integers(n)]
    d2 = ((features - centres[0]) ** 2).sum(1)
    for c in range(1, k):
        total = d2.sum()
        pick = rng.integers(n) if total <= 0 else int(
            np.searchsorted(np.cumsum(d2), rng.random() * total))
        centres[c] = features[min(pick, n - 1)]
        d2 = np.minimum(d2, ((features - centres[c]) ** 2).sum(1))
    labels = np.zeros(n, dtype=int)
    for _ in range(iters):
        dist = ((features[:, None, :] - centres[None, :, :]) ** 2).sum(2)
        new = dist.argmin(1)
        if (new == labels).all():
            break
        labels = new
        for c in range(k):
            member = labels == c
            if member.any():
                centres[c] = features[member].mean(0)
    return labels


def _bone_errors(verts, rest, rot, trans, chunk):
    """Per vertex per bone, the squared error of binding it there rigidly."""
    n, bones = len(rest), rot.shape[0]
    out = np.empty((n, bones))
    for s in range(0, n, chunk):
        sl = slice(s, min(s + chunk, n))
        d = bone_positions(rot, trans, rest[sl]) - verts[None, :, sl, :]
        out[sl] = np.einsum("bfni->nb", d * d)
    return out


def _fit_labels(verts, rest, labels, bones):
    rot = np.empty((bones, verts.shape[0], 3, 3))
    trans = np.empty((bones, verts.shape[0], 3))
    for b in range(bones):
        idx = np.flatnonzero(labels == b)
        if len(idx) < 3:
            rot[b] = np.eye(3)
            trans[b] = 0.0
            continue
        rot[b], trans[b] = procrustes(rest[idx], verts[:, idx, :])
    return rot, trans


def _initialise(verts, rest, bones, seed, chunk, log, passes=8, stride=4):
    """Rigid kinematic clustering: the clusters are the skeleton.

    The feature is the vertex's whole trajectory, subsampled in time. Two
    vertices on one rigid part trace congruent paths, so k-means over
    trajectories already separates a shin from a thigh; the relabelling loop
    then moves every vertex to whichever cluster's fitted rigid motion actually
    reproduces it, which is the criterion that matters and the one k-means only
    approximates.
    """
    feature = verts[::stride].transpose(1, 0, 2).reshape(len(rest), -1)
    labels = _kmeans(feature, bones, seed)
    n = len(rest)
    for step in range(passes):
        rot, trans = _fit_labels(verts, rest, labels, bones)
        err = _bone_errors(verts, rest, rot, trans, chunk)
        new = err.argmin(1)
        # A cluster nobody chose is a wasted bone. Hand it the vertices the
        # current assignment fits worst, which is where a bone is short.
        for b in range(bones):
            if (new == b).sum() < 8:
                worst = np.argsort(-err[np.arange(n), new])[:16]
                new[worst] = b
        moved = int((new != labels).sum())
        labels = new
        if log:
            log("  cluster pass %d: %d vertices moved" % (step, moved))
        if moved == 0:
            break
    rot, trans = _fit_labels(verts, rest, labels, bones)
    return rot, trans, labels


def _solve_weights(verts, rest, rot, trans, candidates, penalty, chunk):
    """Per vertex: non-negative, sums to one, at most MAX_INFLUENCES nonzero.

    The candidate shortlist is the cheap half of the work. A vertex is only
    ever weighted to bones that already reproduce it tolerably on their own, so
    the dense solve is over a handful of columns rather than every bone, and
    the pruning to four influences afterwards has little left to throw away.
    """
    n, bones = len(rest), rot.shape[0]
    out = np.zeros((n, bones))
    for s in range(0, n, chunk):
        sl = slice(s, min(s + chunk, n))
        pos = bone_positions(rot, trans, rest[sl])
        gram = np.einsum("bfni,cfni->nbc", pos, pos)
        rhs = np.einsum("bfni,fni->nb", pos, verts[:, sl, :])
        d = pos - verts[None, :, sl, :]
        err = np.einsum("bfni->nb", d * d)
        for k in range(sl.stop - sl.start):
            cand = np.argsort(err[k])[:candidates]
            w = nnls(gram[k][np.ix_(cand, cand)] + penalty, rhs[k][cand] + penalty)
            if (w > 0).sum() > MAX_INFLUENCES:
                cand = cand[np.argsort(-w)[:MAX_INFLUENCES]]
                w = nnls(gram[k][np.ix_(cand, cand)] + penalty,
                         rhs[k][cand] + penalty)
            total = w.sum()
            if total <= 0.0:
                w = np.zeros_like(w)
                w[0] = 1.0
                total = 1.0
            out[sl.start + k, cand] = w / total
    return out


def reconstruct(verts_shape, rest, rot, trans, weights, chunk=256):
    """The skinned mesh, frame by frame: exactly what tr_surface.c computes."""
    out = np.empty(verts_shape)
    for s in range(0, len(rest), chunk):
        sl = slice(s, min(s + chunk, len(rest)))
        pos = bone_positions(rot, trans, rest[sl])
        out[:, sl, :] = np.einsum("nb,bfni->fni", weights[sl], pos)
    return out


def _update_transforms(verts, rest, rot, trans, weights, recon):
    """Per bone, the transform that best explains what the others left over.

    For bone b the target is the residual with b's own contribution added back,
    divided by its weight - the paper's rearrangement of
    sum_i ||w_i (R p_i + T) - q_i||^2 into a Kabsch problem with weights w_i^2.
    """
    for b in range(rot.shape[0]):
        idx = np.flatnonzero(weights[:, b] > 1e-6)
        if len(idx) < 3:
            continue
        w = weights[idx, b]
        own = (np.einsum("fij,nj->fni", rot[b], rest[idx])
               + trans[b][:, None, :])
        target = verts[:, idx, :] - recon[:, idx, :] + w[None, :, None] * own
        rot[b], trans[b] = procrustes(rest[idx], target / w[None, :, None],
                                      weight=w * w)


def quantise_weights(weights):
    """Round to the renderer's bytes and renormalise, keeping the sum at 255.

    Doing this inside the solve rather than at write time is the difference
    between reporting the residual of the file and reporting the residual of an
    ideal the file cannot hold.
    """
    out = np.zeros_like(weights)
    for i, row in enumerate(weights):
        nz = np.flatnonzero(row > 0)
        if len(nz) == 0:
            continue
        q = np.floor(row[nz] * WEIGHT_SCALE + 0.5).astype(int)
        short = WEIGHT_SCALE - int(q.sum())
        q[int(np.argmax(row[nz]))] += short
        q = np.clip(q, 0, WEIGHT_SCALE)
        out[i, nz] = q / float(q.sum())
    return out


def rms(verts, recon):
    """Root mean square vertex displacement over every frame, world units."""
    d = recon - verts
    return float(np.sqrt((d * d).sum() / (verts.shape[0] * verts.shape[1])))


def frame_rms(verts, recon):
    """Per frame RMS, so an animation range can be scored on its own."""
    d = recon - verts
    return np.sqrt((d * d).sum(axis=(1, 2)) / verts.shape[1])


def solve(verts, rest_frame, bones, iterations=8, seed=0, candidates=8,
          chunk=192, log=None):
    """Fit `bones` rigid bones and skin weights to verts [F, N, 3].

    rest_frame indexes the frame the mesh is authored in. Returns a Solution
    whose weights are already quantised to what the file can store.
    """
    verts = np.asarray(verts, dtype=np.float64)
    if verts.ndim != 3 or verts.shape[2] != 3:
        raise ValueError("verts must be [frames, vertices, 3]")
    frames, count = verts.shape[0], verts.shape[1]
    if bones < 1 or bones > count:
        raise ValueError("bone count out of range")
    rest = verts[rest_frame].copy()

    rot, trans, labels = _initialise(verts, rest, bones, seed, chunk, log)
    weights = np.zeros((count, bones))
    weights[np.arange(count), labels] = 1.0
    # One data row per frame carries unit weight, so a penalty of that order
    # makes sum-to-one about as binding as the whole trajectory.
    penalty = float(frames)

    recon = reconstruct(verts.shape, rest, rot, trans, weights, chunk)
    if log:
        log("  initial RMS %.3f" % rms(verts, recon))
    for step in range(iterations):
        weights = _solve_weights(verts, rest, rot, trans, candidates,
                                 penalty, chunk)
        recon = reconstruct(verts.shape, rest, rot, trans, weights, chunk)
        _update_transforms(verts, rest, rot, trans, weights, recon)
        recon = reconstruct(verts.shape, rest, rot, trans, weights, chunk)
        if log:
            log("  iteration %d: RMS %.3f" % (step, rms(verts, recon)))

    weights = quantise_weights(weights)
    recon = reconstruct(verts.shape, rest, rot, trans, weights, chunk)
    _update_transforms(verts, rest, rot, trans, weights, recon)
    if log:
        recon = reconstruct(verts.shape, rest, rot, trans, weights, chunk)
        log("  quantised: RMS %.3f" % rms(verts, recon))
    return Solution(rest, rot, trans, weights)


# ------------------------------------------------------- skeleton structure

def hierarchy(sol):
    """A parent for every bone, from how rigidly the bones move together.

    The renderer needs a tree and Phase B's bone overrides need one that means
    something, but nothing in the fit constrains it: the bind is the identity
    and poses are written relative to the parent, so any tree reproduces the
    same vertices exactly. The tree is therefore chosen for legibility - a
    minimum spanning tree whose edge cost is how much one bone's origin wanders
    in the other's frame across the animation, tie-broken by rest distance, so
    bones that stay put relative to each other end up parent and child.

    The root is the bone nearest the centre of the rest pose in the horizontal
    plane and lowest in it, which on these rigs is the pelvis.
    """
    bones = sol.bones
    origin = np.einsum("bfij,bj->bfi", sol.rot, _centroids(sol)) + sol.trans
    cost = np.zeros((bones, bones))
    for a in range(bones):
        # b's origin seen in a's frame, frame by frame; its spread is the cost.
        local = np.einsum("fji,fbj->fbi", sol.rot[a],
                          origin.transpose(1, 0, 2) - sol.trans[a][:, None, :])
        cost[a] = local.std(axis=0).sum(axis=1)
    cost = 0.5 * (cost + cost.T)
    mid = _centroids(sol)
    dist = np.linalg.norm(mid[:, None, :] - mid[None, :, :], axis=2)
    cost = cost + 0.05 * dist
    np.fill_diagonal(cost, np.inf)

    span = mid.max(0) - mid.min(0)
    span[span == 0] = 1.0
    norm = (mid - mid.min(0)) / span
    root = int(np.argmin(np.abs(norm[:, 0] - 0.5) + np.abs(norm[:, 1] - 0.5)
                         + np.abs(norm[:, 2] - 0.45)))

    parent = [-1] * bones
    reached = {root}
    while len(reached) < bones:
        best = None
        for a in reached:
            for b in range(bones):
                if b in reached:
                    continue
                if best is None or cost[a, b] < best[0]:
                    best = (cost[a, b], a, b)
        parent[best[2]] = best[1]
        reached.add(best[2])
    return root, parent


def _centroids(sol):
    """Each bone's weighted rest centroid - where the bone sits on the body."""
    w = sol.weights
    total = w.sum(0)
    total[total <= 0] = 1.0
    return (w.T @ sol.rest) / total[:, None]


# Body regions by height through the rest pose, lowest first. A discovered bone
# has no anatomy of its own, so the name states where on the body it sits and
# nothing more - which is what a caller reaching for it by name needs anyway.
BANDS = (
    (0.09, "foot"),
    (0.30, "shin"),
    (0.52, "thigh"),
    (0.62, "hips"),
    (0.78, "chest"),
    (0.87, "neck"),
    (1.01, "head"),
)
# An arm at rest hangs beside the chest, so height alone cannot tell the two
# apart. Anything this far out from the body's mid-line, in fractions of the
# rest pose's half-width, is a limb.
LIMB_FRACTION = 0.45


def region_names(sol, up=2, side=1):
    """One name per bone, from where its rest centroid sits on the body.

    Height picks a band, lateral offset promotes a chest or hips bone to an arm
    and adds an `_l` or `_r`, and bones that land on the same name are numbered
    from the top down so the names are stable for a given fit.
    """
    mid = _centroids(sol)
    lo, hi = sol.rest.min(0), sol.rest.max(0)
    height = hi[up] - lo[up]
    half = max(hi[side] - lo[side], 1e-6) * 0.5
    centre = 0.5 * (hi[side] + lo[side])
    raw = []
    for b in range(sol.bones):
        h = (mid[b][up] - lo[up]) / max(height, 1e-6)
        name = BANDS[-1][1]
        for edge, label in BANDS:
            if h < edge:
                name = label
                break
        off = (mid[b][side] - centre) / half
        if abs(off) > LIMB_FRACTION and name in ("chest", "neck", "hips"):
            name = "arm" if name != "hips" else "hand"
        if name in ("foot", "shin", "thigh", "arm", "hand"):
            name += "_l" if off > 0 else "_r"
        raw.append(name)
    out = []
    for b, name in enumerate(raw):
        same = [i for i, other in enumerate(raw) if other == name]
        if len(same) == 1:
            out.append(name)
        else:
            same.sort(key=lambda i: -mid[i][up])
            out.append("%s%d" % (name, same.index(b) + 1))
    return out


# ------------------------------------------------------------------ selftest

def _self_test():
    rng = np.random.default_rng(7)
    ok = True

    def check(label, cond):
        nonlocal ok
        print("%-46s %s" % (label, "ok" if cond else "FAIL"))
        ok = ok and bool(cond)

    # procrustes recovers a known rigid motion, and refuses a reflection.
    pts = rng.normal(size=(24, 3))
    axis = np.linalg.qr(rng.normal(size=(3, 3)))[0]
    if np.linalg.det(axis) < 0:
        axis[:, 0] *= -1
    shift = rng.normal(size=3)
    rot, trans = procrustes(pts, (pts @ axis.T + shift)[None])
    check("procrustes recovers a rigid motion",
          np.abs(rot[0] - axis).max() < 1e-9 and np.abs(trans[0] - shift).max() < 1e-9)
    flat = np.c_[rng.normal(size=(12, 2)), np.zeros(12)]
    rot, _ = procrustes(flat, (flat @ axis.T + shift)[None])
    check("procrustes stays a rotation on flat input",
          np.linalg.det(rot[0]) > 0.99)

    # nnls against a problem whose answer is on the boundary.
    a = rng.normal(size=(20, 4))
    x0 = np.array([0.0, 1.5, 0.0, 0.5])
    b = a @ x0
    x = nnls(a.T @ a, a.T @ b)
    check("nnls solves a boundary problem", np.abs(x - x0).max() < 1e-6)

    # a two-bone hinge, solved end to end.
    frames = 40
    rest = np.r_[rng.normal(size=(60, 3)) * [1, 1, 2] + [0, 0, -4],
                 rng.normal(size=(60, 3)) * [1, 1, 2] + [0, 0, 4]]
    verts = np.empty((frames, len(rest), 3))
    for f in range(frames):
        ang = 1.1 * np.sin(2 * np.pi * f / frames)
        c, s = np.cos(ang), np.sin(ang)
        top = np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]])
        verts[f, :60] = rest[:60]
        verts[f, 60:] = rest[60:] @ top.T
    sol = solve(verts, 0, 2, iterations=4, seed=1)
    got = reconstruct(verts.shape, sol.rest, sol.rot, sol.trans, sol.weights)
    check("two-bone hinge solves to under 0.01", rms(verts, got) < 0.01)
    check("weights are non-negative and sum to one",
          sol.weights.min() >= 0.0
          and np.abs(sol.weights.sum(1) - 1.0).max() < 1e-9)
    check("no vertex exceeds four influences",
          int((sol.weights > 0).sum(1).max()) <= MAX_INFLUENCES)
    root, parent = hierarchy(sol)
    check("hierarchy is a tree rooted once",
          parent.count(-1) == 1 and parent[root] == -1)
    check("region names are unique", len(set(region_names(sol))) == sol.bones)
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()
    if args.self_test:
        return _self_test()
    ap.error("nothing to do; this is a library. try --self-test")


if __name__ == "__main__":
    sys.exit(main())
