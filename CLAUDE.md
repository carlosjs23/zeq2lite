# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

ZEQ2-Lite — a Dragon Ball Z fighting game built on a fork of the Quake III Arena
engine (id Tech 3, `zeq2lite 1.36`, with ioquake3-era pieces such as the SDL
backend and `Com_QueueEvent`). Plain C, no external build system beyond `make`.

Active work is an **arm64 macOS + SDL2 port** (branch `arm64-macos-sdl2-port`);
`Engine/sdl/*.c` are SDL2 sources even though `Engine/SDL12/` headers are still
in the tree for other platforms.

The ~324MB game data set is **not in this repository**. A playable install lives
under `Build/Release-darwin-arm/ZEQ2/` (`fs_game ZEQ2`); the repo's `Base/` holds
only tracked config. Anything under `Build/` is gitignored build output.

## Commands

```bash
make -j$(sysctl -n hw.ncpu)   # release build (default target); QVMs are NOT built on arm64
make debug                    # into Build/Debug-darwin-$(uname -p)/ — "arm" here, not "arm64"
make lint                     # source-level checks (fast, no build)
make test                     # Criterion unit tests, ASan+UBSan armed
make -C tests msg             # one suite (SUITES: q_shared cg_music cg_players msg weapgfx png snd)
make -C tests list            # every registered test name
make -C tests coverage        # line coverage per file
tests/bin/test_msg --filter 'suite/case'   # one test case (Criterion flag)

Tools/dev/zeq2linux.sh build  # Linux build in a container, incl. the QVMs
Tools/dev/zeq2linux.sh test   # lint + suites on Linux, ASan+UBSan
```

**Always build via `Tools/dev/zeq2build.sh`, not bare `make`.** `make` writes
`Base/cgame$(uname -p).dylib` (`cgamearm.dylib`), but the engine derives module
names from `ARCH_STRING` in `Shared/q_platform.h` — `arm64` — so it loads
`ZEQ2/cgamearm64.dylib`. Bare `make` leaves the engine running stale modules and
your fix appears to do nothing. `zeq2build.sh` stages both names. Pass a module
name (`zeq2build.sh cgame`) to drop its objects first, needed when objects came
from another checkout since `.d` files hold absolute paths.

`Tools/dev/` holds the whole dev loop — `zeq2run.sh` (soak a map for N seconds),
`zeq2smoke.sh` and `zeq2test.sh` (gates), `zeq2shot.sh` (screenshot a settled
frame without Screen Recording permission), `zeq2linux.sh` (Linux build and
tests in a container), `zeq2sanitize.sh`, `zeq2audit.sh`.
**Read `Tools/dev/README.md` before using them**; it documents the sanitizer
flags that must not be dropped and how to read the output. `tests/README.md`
covers the unit-test layout, the stub/fake seams, and the lint rationale.

Running the engine directly needs the mod dir and a windowed mode:

```bash
Build/Release-darwin-arm/ZEQ2.arm +set fs_game ZEQ2 +set r_fullscreen 0 +set r_mode 3 +set com_hunkMegs 256 +map desert
```

## Architecture

Three layers, and knowing which one you are in determines what you can call:

- **`Shared/`** — `qcommon`: filesystem/pk3 (`files.c`), cvars, command buffer,
  `common.c` (event queue, `Com_Frame`), `msg.c` + `net_*` (delta-compressed
  networking), `cm_*` (collision), `vm.c` (module loading). Statically linked
  into the engine *and* into every game module and the renderer — so shared
  globals legitimately exist several times over in one process (this is why
  `detect_odr_violation=0` is required under ASan).
- **`Engine/`** — `client/`, `server/`, `renderer/`, `sdl/` (glimp, input, sound,
  gamma), `sys/` (`sys_main.c` owns `main()`: `IN_Frame()` then `Com_Frame()` in
  a bare loop), `tools/` (the q3lcc/q3asm QVM toolchain), `null/` (dedicated
  stubs).
- **`Game/`** — the mod, compiled twice per build: to QVM bytecode
  (`vm/*.qvm`, interpreted only — arm64 has no bytecode JIT) and to native
  dylibs. `vm_cgame` / `vm_game` / `vm_ui` select which; all default to `0` =
  native dylib. `CGame/` holds the bulk of gameplay presentation (`cg_players.c`,
  `cg_weapons.c`, `cg_particlesystem.c`, auras, beams), `Game/` the server-side
  rules, `UI/` the Team-Arena-style menu system driven by `ui/menus.txt`.

  **On this machine the QVMs are never built.** `HAVE_VM_COMPILED` is unset on
  arm64, which sets `BUILD_GAME_QVM=0` and skips the whole q3lcc/q3asm
  toolchain. x86_64 Linux *does* build them, so anything in `Shared/` or
  `Game/` can compile perfectly here and break there — see
  `Tools/dev/zeq2linux.sh`. The bytecode has no libc: its maths are engine
  syscalls listed in `g_syscalls.asm` (there is `atan2` and no `atan`), and the
  rest of the C library is `bg_lib.c`.

Two boundaries carry most of the porting risk:

**Engine ↔ game modules.** One entry point per module (`vmMain`, dispatched via
`VM_Call`) and one exit (`trap_*` syscalls). A module never links engine
functions directly, so a signature change on either side fails at runtime, not
link time.

**Client ↔ renderer.** The renderer is itself a `dlopen`'d dylib
(`renderer_opengl1_<arch>.dylib`, plus an `_smp` variant) exchanging function
tables through `refexport_t` / `refimport_t` in `Engine/renderer/tr_public.h`.
Consequence that trips people up: **the SDL window is created in the renderer**
(`sdl_glimp.c`) while **input is polled in the client** (`sdl_input.c`), which
recovers the window via `SDL_GL_GetCurrentWindow()` rather than widening the
refimport table. `ri.IN_Init` is called *from* the renderer during `GLimp_Init`.

Input flows `SDL_PollEvent` → `Com_QueueEvent` → `Com_EventLoop`
(`Shared/common.c`) → `CL_KeyEvent` / `CL_MouseEvent` → key catcher (console →
UI → cgame → binds).

## Screen space: widescreen and HiDPI

All 2D drawing is authored in a 640x480 virtual space. `Com_ScreenScale` /
`Com_ScreenAdjustFrom640` (`Shared/q_math.c`) own the mapping onto the
framebuffer and are shared by all three 2D layers — `cl_scrn.c`,
`cg_drawtools.c`, `ui_atoms.c` — which each used to carry their own copy.

Two mappings, and picking the wrong one is the bug to watch for:

- **stretch** fills the framebuffer. Only for things that must cover every
  pixel: console backdrop, cinematics, menu background, the full-screen flash
  and water overlays in `cg_draw.c`.
- **aspect-correct** (default) keeps 4:3 inside a centred box. Everything the
  player reads or aims with. Mixing the two *within* the HUD makes elements
  drift apart as the aspect changes, so a new HUD element wants the same
  mapping as its neighbours.

`glConfig.vidWidth/vidHeight` are **framebuffer pixels**, while SDL window
geometry, `r_customwidth/height` and mouse-warp coordinates are **window
points** — on a Retina display those differ by 2x. Anything that mixes the two
is a bug (`SDL_GL_GetDrawableSize` in `sdl_glimp.c` is where the pixel size
comes from; `r_allowHighDPI` gates it, latched).

`r_mode -2` means "use the desktop resolution" and is the sane default on a
Retina display; `r_mode -1` is `r_customwidth/height`. The table lives in
`tr_vidmodes.c`, deliberately free of renderer state so it can be unit tested —
**its indices are archived in player configs, so append, never renumber.**
`cg_fovAspectAdjust` (default on) treats `cg_fov` as the horizontal fov at 4:3
and widens it for the display, so a wide display shows more at the sides instead
of cropping the top and bottom.

## Port-specific constraints

**Real SDL2 only — never a shim.** Homebrew's `sdl2` formula is now an alias for
`sdl2-compat`, which reaches SDL3 via `dlopen`; that breaks ASan and produces
baffling runtime failures. The Makefile's darwin section picks the newest real
keg under `/opt/homebrew/Cellar/sdl2/`, names the dylib by absolute path,
rewrites the load command after every link and *verifies* with `otool` that no
shim crept in. Override with `SDL2_PREFIX=...` for a from-source build; CI builds
2.32.10 from the upstream tarball for exactly this reason. On Linux this is a
non-problem — distro `libsdl2-dev` is real SDL2 — but the bundled **SDL 1.2**
headers in `Engine/SDL12/include` must stay off the include path there, because
`Engine/sdl/*.c` are SDL2 sources. Windows/mingw still links SDL 1.2 and is
genuinely unported.

**The protocol is stock 71**, so this tree stays wire-compatible with unmodified
ZEQ2-Lite clients, servers and demos. The gameplay work on `combat-and-ai`
widens `powerLevel[]` on the wire and moves to 72; keep that off this branch.

**`Sys_SigHandler` (`Engine/sys/sys_main.c`) installs a `SIGABRT` handler**, so a
stack-protector abort or sanitizer abort surfaces only as
`Sys_SigHandler: caught signal 6` with no diagnostic. A silent `Abort trap: 6`
with a log that just stops is the signature — disable the handler to debug it.

**Config exec order bites.** Startup execs `default.cfg` (all 47 binds) and *then*
the saved `zeq2config.cfg`, which begins with `unbindall`. A config saved from a
session with nothing bound therefore wipes every bind on every launch and rewrites
itself on exit, presenting as "keyboard and mouse do nothing" while Escape and the
console key still work. Check with `+bindlist`, and remember the engine writes the
config back at shutdown.

The same ordering defeats `+set` on the command line: it is applied before the
configs exec, so any `CVAR_ARCHIVE` cvar the saved config mentions —
`s_musicvolume`, `r_picmip`, `cg_thirdPersonSlide`, `model` — silently reverts
to the config's value. Pass it as a bare command instead (`+cg_thirdPersonSlide
0`), which goes into the command buffer and runs after. `+set` still works for
cvars absent from both configs. `Tools/dev/README.md` has the full version.

**The configs are CRLF, and rewriting one in text mode silently converts it.**
`tierDefault.cfg`, the per-character `tier.cfg` files and the rest of the player
configs all ship with CRLF. Python's `open()` translates on write, `awk`'s
`print` emits a bare newline, and most editors will "fix" the file unasked —
none of which looks wrong afterwards, because the engine's parser does not care
and the game runs. It surfaces later: a converted file reports *every* line as
changed against the stock assets, which buries the handful that really did, and
a file with mixed endings (one rewritten key in a CRLF file) shows a scattering
of unrelated-looking lines with no visible cause. Edit these in binary mode —
`open(p,'rb')` / `open(p,'wb')` — or with a tool that matches what the file
already uses, the way `set_key` in `zeq2aura.sh` now does. This has been
introduced several times; assume it will happen again.

`Tools/dev/README.md` and `tests/README.md` list the defects known to be live —
notably the `weaponName` overflow in `cg_weapGfxParser.c`, which fires on every
client load and is what the `check_strncpyz_field_sizes.py` lint exists to catch.

The untracked root `CMakeLists.txt` is not a working build of this port: it wires
no SDL2 and puts the bundled SDL 1.2 headers on the include path. The `Makefile`
is authoritative.

## Conventions

Match the surrounding code: tabs, K&R-ish braces, `qboolean`/`qtrue`, Q3 naming
(`CG_`, `CL_`, `SV_`, `R_`, `trap_`), banner comments over functions. Comments in
this codebase explain *why* — especially where a fix reverses an existing "HACK"
comment — so replace stale reasoning rather than layering on top of it.

Commit subjects are imperative and specific ("Fix corpse tag lookups reading the
living player's pose"). **Keep them to one clause and 60 characters.** A subject
that needs "and" is usually a commit that needs splitting.

Bodies are **3–8 lines**: the mechanism that was wrong, what the change does,
and anything it breaks for other people. That is all. Specifically *not*:

- **investigation narrative** — how the bug was found, what was ruled out, what
  a measurement said before and after. It was worth writing during the session
  and it is noise a year later. A number earns its place only when it is the
  justification for a constant.
- **design argument** — why the mechanic should work this way. That belongs in
  a comment next to the code or in a design note.
- **the story of the previous commit being wrong.** Squash the iteration before
  pushing instead. If a branch contains "move X to Y" and "take X back out of
  Y", the reader wants neither.

No trailers.
