# Aura 1:1 reproduction — session handoff

## Goal

Make the screen-space aura pipeline reproduce the reference art **1:1 — the
entire field, not just the silhouette** — verified by headless measurement
only (never by launching the game). The user's words: "everything should
match 1:1, without actually using the engine."

## Where the work lives

- Worktree: `/Users/carlos/Documents/antigravity/modest-einstein/.claude/worktrees/peaceful-wu-a66acb`
  on branch `ispm/peaceful-wu-a66acb`. **The shell often resets cwd to the
  main checkout between turns — `cd` to the worktree in EVERY command, and
  verify with `git log --oneline -1` before committing (one commit already
  landed on the wrong branch once and had to be reset away).**
- Committed so far (top of branch): `7155334` measurement harness,
  `8e77f54` ground-light fix, `4dddc92` "Bound the flame in the close-camera
  regime" (the state the user called best-previous).
- Branch `aura-baked-outline` preserves an earlier exploration (baked
  outline + auragen strip); parts of it were re-used.
- **Uncommitted work in the worktree right now** (review, then commit in
  repo style — subjects ≤60 chars one clause, bodies 3–8 lines, why-focused):
  - `GameData/glsl/aura_vp.glsl` — baked-outline shaping (restored from the
    side branch), once-around strip sampling (no wrap count), isotropic
    mapping (mockup's 1.3x widen removed), `INNER_HUG 0.0` (band = whole disc).
  - `GameData/glsl/aura_fp.glsl` — samples the strip straight (no mirror
    fold), no mist, neutral boosts (CORE_GLOW 1.0, BASE_GLOW 0.0): the strip
    IS the reference's field, additions are departures.
  - `GameData/scripts/effectsAura.shader` — stage binds
    `effects/aura/auraStrip.png` with clampmapT (restored from side branch).
  - `Tools/dev/make_aura_mesh.py` — outline bake (restored) + channel-aware
    reading (alpha if it varies, else luminance).
  - `Tools/dev/aura_band_from_reference.py` — NEW: unwraps the reference
    into the band strip in the mesh's exact (arc, t) parameterization;
    writes PNG (game) + RAW (harness).
  - `Tools/dev/aurarender.c` — now binds the strip RAW on unit 0
    (REPEAT/CLAMP), composites with the stage's real blend
    (GL_ONE, GL_ONE_MINUS_SRC_ALPHA) over black, outputs the LUMINANCE
    plane (that is what the black-background reference shows).
  - `Tools/dev/aura_silhouette_check.py` — bbox-anchored silhouette curve
    metrics + anchor-free width-per-height profiles + full-field luminance
    diff (`field_stats`, writes a diff PNG next to --plot).
  - `Tools/dev/aura_reference.png` — the ONE reference driving everything
    (outline bake, strip unwrap, all comparisons).

## The architecture (decided, working)

Reference becomes data: `make_aura_mesh.py --outline` bakes the reference's
boundary radius per angle into the ring mesh's vertex-colour alpha (256
segments) with arc-length texcoords; `aura_band_from_reference.py` unwraps
the reference's luminance into a strip along the SAME parameterization; the
vertex program rebuilds the outline and the fragment program samples the
strip at (arc, t) — so at time zero the pipeline reconstructs the reference
nearly by construction. Scroll/wobble animate it in game.

## The measurement loop (~15 s, no engine)

```
cd <worktree> && /tmp/aura-loop/measure.sh
```
(If /tmp/aura-loop is gone, it just wrapped:
`python3 Tools/dev/aura_silhouette_check.py --keep-mask /tmp/aura-loop/mask.png --plot /tmp/aura-loop/plot.png`
plus a pole-radius printout; recreate as convenient.)

Regenerate after changing the bake or unwrap:
```
python3 Tools/dev/make_aura_mesh.py Build/Release-darwin-arm/ZEQ2/models/effects/aura.iqm --segments 256 --outline Tools/dev/aura_reference.png
python3 Tools/dev/aura_band_from_reference.py Tools/dev/aura_reference.png Build/Release-darwin-arm/ZEQ2/effects/aura/auraStrip.png Build/Release-darwin-arm/ZEQ2/effects/aura/auraStrip.raw --inner-hug 0.0
```
(`--inner-hug` must equal `INNER_HUG` in aura_vp.glsl.)

## Metrics right now (CONVERGED)

```
field     : mean|d| 0.0570  rms 0.1373  p95 0.3333   (edge-AA noise mostly)
width rms : 0.0209 (base +0.001, waist -0.001, crown +0.003)
silhouette: rms 0.0267  bias +0.0121  worst -0.1372
tongues   : 52 vs ref 54; depth 0.116 vs 0.120
```
The two big unlocks were removing SHEAR (authored in strip-repeat units;
at one repeat per turn it rotated the field 40 degrees off the geometry)
and removing the wobble harmonics (static +-7.5% outline warp at t=0).
Known nits: a 1px hairline at the wrap seam on the +X flank; the small
centre hole (INNER_HUG 0.05, character covers it in game).

## Immediate next steps

1. **Look at `/tmp/aura-loop/mask.png`** (Read it as an image). The field
   mean barely moved when INNER_HUG went to 0 — suspects: the fp's inner
   fade (`smoothstep(0.0, 0.04, t)`), the inner ring collapapsing to a point
   (all inner verts at r=0 → check the disc actually fills), the field
   metric's bbox alignment, or the strip's t=0 rows. Diagnose visually
   FIRST; earlier rounds wasted iterations on numbers that moved against
   geometry because of measurement artifacts.
2. Drive `field mean|d|` toward ~0.02: region-by-region (the checker writes
   a diff PNG next to --plot — read it; bright = mismatch).
3. Then silhouette worst (−0.30 dropout — find its angle, likely tip/base
   seam or a quantization notch).
4. When the field converges: run ONE in-game check via
   `Tools/dev/zeq2aura.sh --model goku --range 170 --crop none --out <png>`
   (allowed then — the loop itself stays engine-free), because two in-game
   questions are still open: the character stands INSIDE the now-full disc
   (the band may wash over the player depending on depth sorting — vp uses
   nearest-corner depth, sheet draws in front), and the veil sprite in
   cg_auras.c (radius 0.34) may now be redundant since the strip carries
   the interior.
5. Commit in repo style; squash iteration; no trailers.

## Traps already hit — do not re-hit

- **Wrong reference files**: `Gemini_Generated_Image_ea7ckw...png` looks
  like the black reference but has a PAINTED CHECKERBOARD background (fake
  transparency). The true black-background original is what's now saved as
  `Tools/dev/aura_reference.png` (2064x2048, corners are (0,0,0)). The
  transparent Gemini file (`tmwmk3`) has a REAL alpha mask but its alpha is
  a hard shell (opaque to 75% radius) and its RGB interior is checkerboard —
  outline-only use.
- **Measurement anchors drift with the mass they measure**: the silhouette
  radial metrics are bbox-anchored (centroid feedback made numbers move
  against geometry); pole-level causality is still soft — trust the
  anchor-free width profiles and the field diff more.
- CRLF: player configs (tierDefault.cfg etc.) are CRLF — edit in binary
  mode only.
- The engine's GLSL is #version 120; the folklore fract(sin) hash flattens
  on this GPU at large args (aura_fp has a bounded-arg hash if noise is
  ever needed again).
- aurarender.c hardcodes the uniform contract (box, scale 1.2, wavelength,
  camera range 160/fov 75) — keep in step with tierDefault.cfg by hand.
