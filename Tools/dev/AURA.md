# The 1:1 aura: from one reference PNG to the in-game effect

The screen-space aura reproduces a piece of reference art exactly — the whole
field, not just the silhouette — and every asset it draws is generated from
that one image. This documents the pipeline, the measurement loop that keeps
it honest, and the recipe for building a new aura (a tier's, a character's)
from a new reference.

## The idea

Reference becomes data. The art's silhouette is baked into the ring mesh as a
radius per angle; the art's interior is unwrapped into a band strip along the
same parameterization; the vertex program rebuilds the outline against the
player's screen-space bounding box and the fragment program samples the strip
at (arc, band) — so at rest the pipeline reconstructs the reference nearly by
construction, and everything else (sway, flicker, motion response) is built
to vanish at that rest pose.

## The reference image

`Tools/dev/aura_reference.png`. Requirements:

- **High resolution** (the current one is 2064x2048): the strip and outline
  inherit its detail, and needle-fine licks vanish below ~2px.
- **Silhouette in alpha, or art shot on black.** Every reader in the pipeline
  uses the same channel rule: alpha if it varies, else luminance.
- **One connected, star-shaped blob**: the outline is a radius per angle from
  the field's centroid, so every ray must cross the boundary once. No
  detached wisps, no licks folding back over themselves.
- Beware AI-generated "transparent" images whose checkerboard is painted into
  the pixels — it bakes straight in. Corners must be actual black/transparent.
  `aura_reference_clean.py` repairs all three delivery forms (real alpha,
  black background, painted checker) into the canonical luminance-on-black.
- References do not have to be found or painted: `auragen.c` generates them
  procedurally (seeded, deterministic) — one seed per tier is a cheap way to
  give every transformation its own field. Its canvas is 1024x1280, half the
  scale the pipeline was tuned on, so fine strand grain softens; bump its
  canvas constants for production bakes.
- The reference is the aura **without the character**: the game draws the
  real model in the middle, so art that shows a figure inside the flame must
  have the figure's space left open (a pale interior column is fine — the
  character covers it).

## Generation (two scripts, `zeq2build.sh` is the argument authority)

```
python3 Tools/dev/make_aura_mesh.py <build>/ZEQ2/models/effects/aura.iqm \
        --segments 1024 --outline Tools/dev/aura_reference.png
python3 Tools/dev/aura_band_from_reference.py Tools/dev/aura_reference.png \
        <build>/ZEQ2/effects/aura/auraStrip.png <build>/ZEQ2/effects/aura/auraStrip.raw \
        --inner-hug 0.0 --segments 1024 --width 2048 --height 512 --frames 4
```

`make_aura_mesh.py` casts rays from the art's centroid, stores each vertex's
direction and boundary radius **as floats in the normal stream** (a colour
byte's 1/127 step is coarser than the segment spacing — bytes aliased
directions and drew needles at their neighbours' angles), and advances the
texture coordinate with the outline's own arc length.

`aura_band_from_reference.py` unwraps the art into the strip along the same
rays: column = arc position, row = fraction of the boundary radius. Its inner
rows blend to their angular mean (a polar field has no angular information at
its centre — per-angle sampling there magnified a few pixels into wedge
spokes), and `--frames` stacks lick-jittered variants of the band for the
flipbook (frame 0 is always the reference exactly).

The arguments live in one place — the aura section of `Tools/dev/zeq2build.sh`,
which regenerates both assets on every build. `--inner-hug` must equal
`INNER_HUG` in `aura_vp.glsl`; `--frames` must equal `STRIP_FRAMES` in
`aura_fp.glsl`; `--segments` must match between the two scripts. A stale
recipe in a doc once cost a session an hour: trust the build script.

## Runtime (what consumes the assets)

- `GameData/glsl/aura_vp.glsl` — rebuilds the outline against the projected
  player box; reprojects every vertex to an honest world depth (behind the
  character, folded onto the feet plane where that is nearer, so the player
  renders clean and the skirt lies across the floor); fans the strip's u
  projectively through each wedge; sways the field around home; and answers
  motion head-first — the crown follows the head, proportional to speed and
  to how much of the velocity the screen can see.
- `GameData/glsl/aura_fp.glsl` — samples the strip (optionally flickering
  through its frames), tints from the entity colour, premultiplied output.
- `GameData/scripts/effectsAura.shader` — the `Aura_ScreenSpace` stage.
- `Game/CGame/cg_auras.c` — fills `programParams[16]` per entity; the layout
  is the header comment in `aura_vp.glsl` and nothing validates the two
  agree, so change them together. `[3].w` carries motion (0 standing, 1 at
  full boost). The path is gated by `cg_auraScreenSpace`.

## The measurement loop (~10 s, no engine)

```
python3 Tools/dev/aura_silhouette_check.py --keep-mask /tmp/mask.png --plot /tmp/plot.png
```

`aurarender.c` runs the real mesh, real shaders and real strip in a headless
GL context at the reference's own scale and writes the frame's alpha;
the checker measures mask and reference identically: a full-field luminance
diff (the 1:1 number), anchor-free width profiles, boundary curves, tongue
statistics. Current state: field mean|d| ≈ 0.049, tongue count matching the
art. `aurarender` hardcodes the uniform contract (box, scale, camera) — keep
it in step with `tierDefault.cfg` by hand.

Two habits that earned their place. **Diagnose visually before chasing a
number** — the diff PNG written next to `--plot` maps the mismatch, and
numbers have moved against geometry before (anchors drifting with the mass
they measure). And when the number and the picture disagree about *where* the
error lives, render the fragment stage's own interpolants — a variant fp
whose output is `tt` or `uu` as luminance — and compare against the ideal
mapping: that split proves whether geometry or texturing is lying (it proved
texturing exact and pinned the byte-payload aliasing).

## Animation and the clip loop

Animation is judged on motion, so it has its own loop: `zeq2clip.sh` records
a scripted demo once (`--input` drives flight/boost/movement — key commands
cannot ride the engine command line, which strips their +/- prefixes) and
replays it through shader overlays, producing clips that differ by exactly
the shader. `Tools/dev/aura_variants/` holds the animation candidates; the
shipped default is the sway (`b-sway`), and every animation term is built to
vanish at time zero so the measured rest frame stays the reference.

## A new aura from a new reference

1. Save the reference (requirements above) — e.g. `aura_reference_ssj3.png`.
2. Run the two generation commands against it, into per-tier asset paths.
3. Point a shader stage at the new strip (tiers already take `auraShader`),
   and register the per-tier mesh where `cg_auras.c` registers `aura.iqm`.
4. Duplicate the measurement loop with `--reference` pointed at the new art
   and converge: silhouette first, then the field, visually diagnosing
   between runs.
5. Colour: the strip carries the art's own colours over black, and
   `auraColor` multiplies them - white shows the art as painted, a tint
   shifts it. A greyscale reference behaves exactly as the old white-strip
   system did, so recolouring a colourless aura still needs no rebake; use
   `aura_reference_clean.py --color` to keep a coloured reference's hues.

## Traps (all hit once already)

- Player configs are CRLF; edit binary-safe or the diff drowns (README trap 4).
- The engine command line strips `+`/`-` command prefixes; scripted input
  must go through an exec'd cfg (`zeq2clip.sh --input`).
- Byte-quantized per-vertex payloads alias at high segment counts; the mesh
  carries floats in the normal stream for a reason.
- Regeneration arguments drift when documented in more than one place;
  `zeq2build.sh` is the authority, everything else refers to it.
