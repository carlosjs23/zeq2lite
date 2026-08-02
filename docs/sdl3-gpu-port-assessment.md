# Porting to SDL3 + SDL_GPU — effort assessment

Written against the tree at `ispm/sdl3-metal-renderer-73e4b3`. Every number here
was measured from the source, not estimated from the outside.

## Correction to the first impression

The renderer looks like a 30,713-line fixed-function OpenGL program, and on a
first pass that is what it reads as: 424 distinct `qgl*` entry points, immediate
mode, a matrix stack, texture-env combiners, client arrays. On that reading the
port is a ground-up rewrite and the answer is "months, don't".

That reading is wrong, and the correction is the single most important fact in
this document:

```
renderer source                     30,713 lines
lines containing a GL call             593 lines   (1.9%)
files that never touch GL            21 of 35
```

Q3 does all per-vertex work — `tcMod`, `rgbGen`, `alphaGen`, `deformVertexes`,
skeletal animation, lighting, curve tessellation — **on the CPU**, into one
contiguous batch (`tess` / `shaderCommands_t`). By the time anything reaches the
driver it is a finished, flat triangle list with positions, normals, two UV sets
and vertex colours. The GPU is handed a completed batch and told to draw it.

That is precisely the shape SDL_GPU wants. The port is not a rewrite of the
renderer; it is a replacement of the ~600-line bottom edge of it.

## What is already done for you

**There is a working GLSL path, and it is the one that actually runs.**
`r_ext_vertex_shader "1"` is set in the live `zeq2config.cfg`. The engine has
`RB_GLSL_StageIteratorGeneric`, `…Sky`, `…VertexLitTexture`,
`…LightmappedMultitexture`, a full uniform system (`R_GLSL_SetUniform_*`), and
Q3 shader scripts bind programs by name via the `program` keyword. 18 GLSL files
totalling 1,521 lines ship in the game data, including the cel-shading and aura
programs that give ZEQ2 its look.

This matters more than it first appears. The hard, judgement-heavy part of a
port like this is re-deriving what each fixed-function stage combination was
*supposed* to compute. That work has already been done and debugged here.
Translating an existing, correct GLSL shader to HLSL is mechanical.

**`cl_renderer` already exists.** `cl_main.c:3082` loads
`renderer_<name>_<arch>.dylib` and falls back to `opengl1` if the named one
fails to load. Shipping both renderers side by side, cvar-selected, needs
essentially no new engine plumbing — only a second Makefile target.

## Work breakdown

### Phase 0 — SDL2 → SDL3 (prerequisite, independent of the renderer)

~180 SDL symbols in `Engine/sdl/*.c` (3,008 lines). Most are mechanical renames.
The parts that need real thought:

- **Audio.** `SDL_OpenAudio` + pull callback → `SDL_OpenAudioDeviceStream`. The
  callback signature and threading model both changed. `sdl_snd.c` rewrite.
- **Events.** `SDL_WINDOWEVENT` + `event.window.event` sub-type collapses into
  flat `SDL_EVENT_WINDOW_*` types. Touches every case in `sdl_input.c`.
- **Joystick.** Index-based → `SDL_JoystickID`; `SDL_NumJoysticks` is gone in
  favour of `SDL_GetJoysticks`.
- **Gamma ramps are deleted from SDL3.** `SDL_SetWindowGammaRamp` /
  `SDL_SetWindowBrightness` no longer exist, so `sdl_gamma.c` loses its
  hardware path entirely. Survivable: this branch already moved macOS to
  software gamma because the ramp was being ignored anyway.
- **Displays.** Display *index* → `SDL_DisplayID`; `SDL_GetNumDisplayModes` /
  `SDL_GetDisplayMode` → `SDL_GetFullscreenDisplayModes`. Affects
  `tr_vidmodes.c`'s consumers (the table itself is fine, and its indices are
  archived in player configs — still append-only).
- **HiDPI.** `SDL_GL_GetDrawableSize` → `SDL_GetWindowSizeInPixels`. The
  points-vs-pixels distinction documented in CLAUDE.md is unchanged, but every
  call site moves.
- `SDL_bool`/`SDL_TRUE` → plain `bool`/`true`; most `SDL_Init`-family functions
  return `bool` instead of `int` with inverted sense — an easy silent-breakage
  class worth a careful pass.
- Makefile: SDL3 is at `/opt/homebrew/Cellar/sdl3/3.4.12` and is real SDL3 (the
  shims are the separate `sdl2-compat` / `sdl12-compat` kegs). The existing
  anti-shim `otool` verification should be kept and retargeted.

**Estimate: 1–2 sessions.** Bounded, verifiable, and leaves the GL renderer
working throughout.

### Phase 1 — SDL_GPU device and swapchain

Replace GL context creation in `sdl_glimp.c` (1,515 lines) with
`SDL_CreateGPUDevice` + `SDL_ClaimWindowForGPUDevice`. This is *simpler* than
what it replaces — no pixel-format attribute negotiation, no context-profile
dance, no `SDL_GL_SetAttribute` ladder with fallbacks.

**Estimate: 1 session** to a cleared screen.

### Phase 2 — The pipeline cache (the one genuinely new subsystem)

`GL_State(stateBits)` in `tr_backend.c` translates a 32-bit mask into blend,
depth, cull and polygon-mode changes, mutating global state per draw. SDL_GPU
has no mutable state: all of it is baked into an immutable
`SDL_GPUGraphicsPipeline` at creation time.

So this becomes a hash from `(stateBits, cullType, program, vertex layout,
target format)` to a lazily-created pipeline. The 25 `GLS_*` bits look
combinatorially frightening but in practice only a few dozen blend combinations
occur across the shipped shaders; expect 100–400 live pipelines, built on
demand.

Two specific traps:

- **There is no alpha test.** `GLS_ATEST_GT_0` / `LT_80` / `GE_80` must become
  `discard` in the fragment shader — either three shader variants or one
  uniform-driven branch.
- **There is no `glPolygonMode`.** `GLS_POLYMODE_LINE` (used by `r_showtris`)
  needs a line-topology pipeline or a geometry-free wireframe substitute.

**Estimate: 1–2 sessions.** ~500–800 new lines.

### Phase 3 — Geometry submission

`R_DrawElements` and the `qglVertexPointer` / `qglTexCoordPointer` /
`qglColorPointer` setup around it become a per-frame transfer buffer copied into
GPU vertex and index buffers. Because `tess` is already one contiguous CPU
batch, this is a memcpy per draw, not a restructuring.

Worth noting this is a *performance improvement*, not a tax: the GL path already
re-uploads client arrays on every draw call, implicitly and unbatchably.

**Estimate: ~200 lines, folds into Phase 2's session.**

### Phase 4 — Shaders and the cross-platform toolchain

The 18 existing GLSL files are `#version 120` and lean on compatibility
built-ins — `gl_Vertex`, `gl_Color`, `gl_Normal`, `gl_MultiTexCoord0`,
`ftransform()`, `gl_FragColor`, `gl_FrontColor`. None of those exist in a
SDL_GPU shader, so every file needs its inputs and outputs made explicit. The
*logic* survives; the interface does not.

Then the paths that currently rely on fixed function need shaders written from
scratch: 2D/`RB_StretchPic`, the `GL_MODULATE`/`ADD`/`DECAL`/`REPLACE` combine
paths, dlight projection, fog, sky, flares, shadow volumes, bloom (4 passes),
motion blur, and the debug tris/normals views. Call it **25–35 shader pairs**
total.

**The toolchain is the real cost of the cross-platform requirement.** SDL_GPU
consumes SPIR-V (Vulkan), DXIL/DXBC (D3D12) and MSL/metallib (Metal). It does
**not** translate between them — Metal will not accept SPIR-V. So shaders must
be authored once (HLSL is the pragmatic choice) and compiled offline into all
three formats via SDL_shadercross (DXC for DXIL/SPIR-V, SPIRV-Cross for MSL).
`shadercross` is not installed and becomes a new build dependency pulling in DXC
and SPIRV-Cross. The generated blobs should be committed so end users never need
the toolchain.

> **The sharpest constraint in the whole project.** Q3 shader scripts can name
> *arbitrary* GLSL programs loaded from game data at runtime (`program cel`).
> Under SDL_GPU there is no runtime GLSL compilation, so this extension point
> stops working unless you bundle glslang + SPIRV-Cross into the renderer.
> For *this* game that is currently acceptable — only four programs are
> referenced (`skip`, `cel`, `celdynamic`, `aura`) and all four ship with the
> game, so they can be precompiled. But it is a real change to the modding
> model and should be a deliberate decision, not a discovery made late.

**Estimate: 1–2 sessions for the toolchain, 3–5 for the shader bodies.**

### Phase 5 — The offscreen frame graph

`tr_bloom.c` and `tr_motionblur.c` currently grab the backbuffer with
`qglCopyTexSubImage2D` (14 such readback sites across the renderer). SDL_GPU has
no read-from-swapchain. The frame has to become: render scene → offscreen colour
target → post chain → swapchain. That is a structural change to `tr_cmds.c` and
`tr_backend.c`, not just to the two effect files.

Silver lining: software gamma also becomes a post-process pass here, which is
cleaner than what `sdl_gamma.c` does today.

**Estimate: 1–2 sessions.**

### Phase 6 — Parity chase

Blend-mode mismatches, tcMod edge cases, alpha-test cutoff differences, sort
order, and above all **getting the cel-shading and aura look pixel-identical**.
That last item is the game's visual identity and the most bespoke shader code in
the tree.

**Estimate: 3–5 sessions.** This is always the long tail and it is always the
part that gets underestimated.

## Totals

| Phase | Sessions |
|---|---|
| 0 — SDL2 → SDL3 | 1–2 |
| 1 — Device / swapchain | 1 |
| 2+3 — Pipeline cache, buffers | 2–3 |
| 4 — Shaders + toolchain | 4–7 |
| 5 — Offscreen frame graph | 1–2 |
| 6 — Parity chase | 3–5 |
| **Total to macOS parity** | **12–20** |

Windows and Linux add more, unevenly:

- **Linux** (SDL_GPU → Vulkan) is testable here via `Tools/dev/zeq2linux.sh`,
  though the container needs a Vulkan-capable setup it does not currently have.
- **Windows** (SDL_GPU → D3D12) cannot be validated from this machine at all,
  and per `CLAUDE.md` the Windows/mingw build is *still on SDL 1.2 and
  genuinely unported* — so Windows needs the whole SDL3 migration done from
  scratch first, independent of the renderer work.

## What you actually get — measured, not assumed

An earlier draft of this document asserted the game was "CPU- and draw-call-bound,
not GPU-bound". **That was wrong**, and measuring it says something more useful.

### Method

Apple M4, `desert`, fullscreen at 1470x956, `r_swapInterval 0` (vsync off —
already the default), `com_maxfps 0`. Combat is a real fight via
`Tools/dev/zeq2duel.sh --skill 5` (goku vs vegetaCell), sampled after the
fighters engage. `com_speeds 1` gives the frame breakdown, `r_speeds 7` splits
the backend into swap-blocked versus submit, `r_speeds 1` gives geometry, and
`r_measureOverdraw 1` (which needs `r_stencilbits 8` — the default pixel format
has none) counts fragments per pixel via `GL_INCR` on the stencil.

### Results, in combat

```
frame          10.43 ms   (96 fps)      worst 5% of frames: 20.1 ms (50 fps)
  client        1.11 ms
  game          0.08 ms
  frontend      0.15 ms   <- renderer CPU: culling, tess building, tcMod/rgbGen
  backend       9.02 ms
      swap      7.15 ms   <- GPU busy (vsync is OFF, so this is not idle)
      submit    1.88 ms

draw calls      140 per frame (max 249)
geometry        142 shaders, 445 surfaces, 21.6k verts
triangles       24k drawn, 38k submitted  (~1.6x multi-pass)
OVERDRAW        20.8x average, 37.4x peak
```

### What that means

**The frame is fill-bound, and nothing else is close.** 1470x956 is 1.41M
pixels; at 20.8x overdraw that is **~29 million fragment shader invocations per
frame**, ~2.8 billion per second at 96 fps, every one of them blended and most
doing dependent texture reads. Meanwhile the things a new API is good at fixing
are already free: 140 draw calls is nothing, 24k triangles is nothing for an M4,
and the renderer's CPU frontend costs **0.15 ms** — 1.4% of the frame.

The 20.8x is not a bug to be fixed by a driver. It is what the game *is*: auras,
beams, charge effects and particles are additive transparent layers stacked over
each other, and transparent fragments cannot be eliminated by the hidden-surface
removal that Apple's tile-based GPU does for opaque geometry. Porting to Metal
does not remove a single one of those 29 million invocations.

**So: the conclusion survives, for a better reason than I first gave.** Not
"it's CPU-bound so the GPU API doesn't matter" — it's that the GPU is genuinely
saturated doing work the API change does not reduce.

### What a Metal port could honestly recover

- **Most of the 1.88 ms submit.** That is real CPU cost — GL validation, state
  translation, and the client-array re-upload on every draw. Pipeline objects
  and explicit buffers should take most of it. Call it ~1.3 ms.
- **Some unknown share of swap.** macOS OpenGL is itself implemented on Metal,
  so there is a translation layer in there. Not zero, not measurable from
  outside, and not the fragment work.
- **Realistically ~15–20% of frame time**, so roughly 96 → 110-115 fps. A real
  improvement. Not a doubling, and not what "native Metal" tends to suggest.

### The biggest win is available in the GL renderer today, without any of this

Bloom (`r_bloom`, default **1**, on in the shipped config) costs **~10 ms of a
~14 ms frame**. Turning it off takes the static desert scene from ~70 fps to
**265 fps** — a 3.7x difference, far outside the ±15% run noise.

What makes that actionable is *where* the cost is not. Measured with the config
restored between every run (see the pollution caveat below):

```
stock (darken 32, diamond 8, sample 512)   14.20 / 11.78 / 15.74 ms   ~70/85/64 fps
bloom OFF                                   3.77 ms   265 fps
darken 32 -> 1     (removes 31 of ~99 passes)   12.99 ms   within stock variance
diamond 8 -> 4     (64 -> 16 blur taps)         15.13 ms   within stock variance
sample 512 -> 256  (quarter the work area)      15.26 ms   within stock variance
```

**None of the blur knobs matter.** Removing a third of the passes, quartering the
tap count and quartering the work-buffer area each changed nothing measurable.
So the cost is not fragment work and not the blur algorithm.

Varying screen area instead:

```
1470x956   ON 14.20   OFF 3.77   -> bloom costs 10.4 ms
 640x480   ON 13.93   OFF 7.14   -> bloom costs  6.8 ms
```

Screen area fell 4.6x; bloom's cost fell 1.5x. That leaves a **~6.8 ms component
independent of screen area, pass count and buffer size** — the signature of a
fixed number of pipeline flushes, roughly 0.75 ms each. `tr_bloom.c` issues
exactly nine framebuffer→texture copies per frame (seven of the work buffer, one
full-screen backup at `glConfig.vidWidth/Height`, one restore), and on a
tile-based deferred GPU each `glCopyTexSubImage2D` forces the tile buffer to
resolve to memory and synchronise. Nine of those is the whole cost.

**The fix is FBOs, in the existing GL renderer.** Render the passes into a
framebuffer object instead of copying out of the backbuffer nine times. Note
`qgl.h` had **no framebuffer entry points at all** — they had to be added first,
which was unremarkable: `ARB_framebuffer_object` is core in GL 3.0 and present
in macOS's 4.1 context.

#### What that actually bought (done — and the prediction above was too optimistic)

Implemented: the passes now alternate between two textures bound to an FBO, and
only one backbuffer copy survives (the screen grab the downscale samples from).
Verified visually — at a camera pinned with `setviewpos`, stock and FBO output
differ by mean |d| 7.7 per channel, while **two runs of the *stock* renderer
differ from each other by 38.7**. The change is well inside the harness's own
noise.

Measured properly with `Tools/dev/zeq2bench.sh --ab r_bloom_fbo 1 0`, which
replays a recorded demo so every run draws the same 1139 frames:

```
  FBO ms  copy ms   ratio    machine state
    9.60    12.22    1.27x    quiet
    9.68    12.27    1.27x    quiet
   12.84    15.93    1.24x    medium
   15.97    15.47    0.97x    saturated
   16.01    16.60    1.04x    saturated
   16.32    16.43    1.01x    saturated
   16.55    17.19    1.04x    saturated
   16.90    16.92    1.00x    saturated
```

**Render-to-texture is ~1.20x on frame time** (median 1.199x, range
1.11-1.27, sd 0.065 over seven cold pairs). The `r_bloom_fbo` cvar exists so the
two paths can be A/B'd against each other; it is not something a player needs to
touch.

Worth recording how that number moved, because it is a lesson about sample size
rather than about the renderer. The first three cold pairs read 1.27, 1.27,
1.25 and looked reproducible to 0.02x. Four more, this time gated on 1-minute
load *and* a thermal settle, read 1.17, 1.13, 1.11, 1.20. The first three had
simply clustered — three samples cannot distinguish "tight" from "lucky", and
quoting a two-decimal figure off them was overconfident. The FBO arm itself is
the stable one across all seven (9.28-10.04 ms); nearly all the spread is in the
copy arm.

**Only the first pair of a session is worth reading.** Sustained fullscreen
benchmarking heats the machine until `kernel_task` ramps to 190%+ forcing a
thermal throttle — load hit 37 *during* an eight-run A/B — so later pairs
measure the governor, not the renderer. This is self-inflicted by the benchmark
and it is why a long `--runs` sweep converges on "no effect": every arm ends up
throttled to the same wall. Prefer several cold `--runs 1` invocations spaced
apart over one long sweep.

The lower rows are not a contradiction, they are the measurement failing. The
saving is GPU-side, so it only shows while the engine can actually run: with
~19 busy `claude-code` processes taking about 491% CPU on a 10-core machine
that has only 4 performance cores, both arms hit the same CPU wall around
16.5 ms and the difference vanishes. The ratio degrades monotonically with load,
which is the signature to watch for — **a renderer A/B on a loaded machine
reads as "no effect" no matter how large the effect is.** `r_swapInterval 0`
and the timedemo path (which ignores `com_maxfps`) are both already pinned by
the harness, so that wall is not vsync or a frame cap.

Not wasted either way: Phase 5 of the port needs this same offscreen structure,
and rendering the *scene* into an FBO — which would remove the last copy — is
the natural next step.

Two honesty caveats. First, this excludes the alternatives rather than
instrumenting the copies directly; timing them individually would make it
conclusive. Second, the 640x480 bloom-OFF figure (7.14 ms) is *slower* than the
1470x956 bloom-OFF figure (3.77 ms), which should be impossible — its submit
time doubled, so something in that run is confounded. It does not affect the
bloom-on-vs-off deltas, but it is unexplained.

### Where bloom's remaining cost is

On the benchmark demo, with render-to-texture already in:

```
r_bloom 1          9.69 ms (103 fps)
r_bloom 0          4.21 ms (237 fps)     ratio 0.43x, range 0.42-0.45
r_bloom_darken 32 vs 1                   ratio 1.04x, range 1.01-1.06
```

**Bloom is 5.47 ms of a 9.69 ms frame — 56% of frame time**, and dropping 31 of
its ~99 blend passes changes nothing. Neither does the tap count or the work
buffer's area. The blur is not the cost and rewriting it as a separable Gaussian
would buy nothing.

What is left runs once per frame and is proportional to screen area: the
full-resolution `R_Bloom_BackupScreen` grab of the backbuffer, and the
full-screen additive `R_Bloom_DrawEffect`.

#### Done — the grab was nearly all of it

The scene now renders into `screen.texture` directly (`r_bloom_fbo 2`, the
default), so the downscale samples what is already there and the grab is gone.
The frame ends by drawing that texture to the backbuffer, which the copy path
had to do anyway as a restore.

```
copy path    bloom on 9.69 ms   off 4.21 ms   -> bloom costs 5.47 ms
scene FBO    bloom on 4.54 ms   off 3.82 ms   -> bloom costs 0.72 ms
```

**Bloom went from 5.47 ms to 0.72 ms, and the frame from 9.69 ms to 4.54 ms**
(103 -> 220 fps). One full-resolution `glCopyTexSubImage2D` per frame was
essentially the whole effect's cost — more than the 99 blend passes, the blur
taps and the work-buffer area put together, none of which measure at all.

Verified on a fixed demo frame, where all three paths agree: `fbo0` vs `fbo1`
mean |d| 0.061, `fbo0` vs `fbo2` 0.108, luminance 162.919 / 162.918 / 162.920.

That verification matters more than it looks. A first attempt used `setviewpos`
to pin the camera for the comparison, and it silently failed to take in some
runs — two of the three shots were of a different part of the map, which read as
"the effect is not being applied at all" and nearly got a correct change
reverted. Screenshot A/Bs need the demo, for the same reason the timings do.

This is also the offscreen frame graph Phase 5 of the port needs, so the port
inherits it rather than having to build it.

### Methodology warning: archived cvars silently poison A/B runs

Every `r_bloom*` cvar is `CVAR_ARCHIVE`. A `+set r_bloom 0` on the command line
is written back into `zeq2config.cfg` at shutdown and then applies to **every
later run**. A first pass at the table above was entirely invalid for this
reason: three "tuning" runs were measuring bloom-off and looked like enormous
wins. Any benchmark harness must restore a pristine config before each run and
state every cvar it cares about explicitly. `Tools/dev/README.md` documents the
general hazard; this is the renderer instance of it.

### The one place the port genuinely helps performance

Apple GPUs are tile-based deferred. `tr_bloom.c` and `tr_motionblur.c` currently
pull the backbuffer with `qglCopyTexSubImage2D` mid-frame, which forces a tile
flush — close to a worst case on TBDR hardware. Phase 5 replaces that with an
explicit offscreen frame graph because SDL_GPU leaves no alternative, and
correct load/store actions plus memoryless depth are likely worth more than the
API switch itself.

Worth being precise about the causality: that win comes from the
**restructuring the port forces**, not from Metal. You could do the same
offscreen frame graph in the existing GL renderer and collect most of it without
any of this work.

### If framerate is the actual goal

Overdraw is the whole game, and every lever on it is API-independent: fewer aura
layers, effect geometry tightened to actual coverage instead of oversized quads,
culling offscreen and backfacing effect billboards, and collapsing the 1.6x
multi-pass into single-pass programs (the GLSL path already does some of this).
That work would beat the entire port on framerate, at a fraction of the cost.

**Do the port for the deprecated-API problem, the tooling and the correctness —
not for the frames.**

### Main risk

The cel-shading/aura pipeline is bespoke, is the game's identity, and is the
hardest thing to reproduce exactly. Budget for it explicitly rather than
assuming it falls out of the general port.

### Caveats on these numbers

Both AI fighters spawn and the camera spectates, but there is no fixed camera
path, so per-run variance is high — draw counts ranged 58–249 across runs
depending on what was on screen. Windowed runs add a ~4.9 ms compositor floor to
swap and should not be used for this (`zeq2env.sh` warns about exactly that);
every number above is fullscreen. The overdraw figure comes from a run with
full-frame stencil readback enabled, which destroys frame timing — the *ratio*
is valid, the fps during that run is not.

## Recommended sequencing

1. **Phase 0 alone, landed and verified.** SDL3 is a hard prerequisite for
   SDL_GPU, it is independently valuable, and it keeps the game playable. If the
   larger project stalls, this is still a win rather than a dead branch.
2. **Then Phases 1–3** as a spike: a second `renderer_sdlgpu_<arch>.dylib`
   selected by the existing `cl_renderer` cvar, drawing the world with the
   generic stage iterator and nothing else. This is the point at which the
   remaining estimate stops being a guess, because the pipeline cache and the
   shader toolchain — the two genuine unknowns — are both proven or disproven by
   then.
3. **Reassess before committing to Phases 4–6.**

Keeping `opengl1` as the default and shipping `sdlgpu` opt-in until it reaches
parity costs almost nothing given `cl_renderer` already exists, and it is what
makes an incremental port possible at all.
