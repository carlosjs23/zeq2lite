# Tools/dev — deterministic dev loop for ZEQ2-Lite

Small, dependency-free scripts for the build → run → look-at-a-frame loop, so
that verifying a change never means re-deriving throwaway shell one-liners.
Python scripts use only the standard library; shell scripts need bash.

| script | what it does |
| --- | --- |
| `zeq2build.sh` | `make`, then stage the game modules where the engine loads them |
| `zeq2run.sh` | join a map, stay alive N seconds, report how the run ended |
| `zeq2shot.sh` | join a map, grab an in-engine screenshot, convert it to PNG |
| `zeq2smoke.sh` | **gate**: load every map, assert the game survives joining it |
| `zeq2audit.sh` | report where the code expects assets the data set never shipped |
| `zeq2aura.sh` | sweep the aura's tuning and contact-sheet what each value renders as |
| `zeq2clip.sh` | record a demo once, replay it through shader variants as video clips |
| `zeq2sanitize.sh` | build + run under ASan/UBSan and group the findings |
| `zeq2test.sh` | **gate**: static checks, ASan demonstrations, sanitizer-log assertions |
| `tga2png.py` | convert an ioquake3 TGA screenshot to PNG (+ colour histogram) |
| `png_sheet.py` | flatten PNGs onto one background so transparent art can be looked at |
| `make_ui_art.py` | generate the interface images the data set never shipped |
| `make_aura_mesh.py` | generate the screen-space aura's ring mesh |
| `make_aura_texture.py` | generate the screen-space aura's spike strip |
| `zeq2env.sh` | shared paths/helpers, sourced by the others (not run directly) |

`zeq2smoke.sh` and `zeq2test.sh` are gates (non-zero exit on failure). The rest
are reports.

## Regression tests (`zeq2test.sh`)

`zeq2test.sh` with no arguments runs only the static checks — fast, no build.
`--demo` adds the ASan demonstrations, `--asan` asserts the last
`zeq2sanitize.sh` run was clean, `--all` does everything.

Tests live in `tests/`. Two kinds, and the distinction matters:

- **`check_*.py` — gates.** They assert an invariant and flip from fail to pass
  when the defect is fixed. Safe to wire into CI.
- **`demo_*.c` — demonstrations.** They prove a specific defect is real by
  reproducing it under ASan against the engine's own headers and functions. A
  demonstration *fails while the defect exists* and passes once it is gone, so it
  doubles as a gate — but its real value is the diagnostic it prints, which shows
  the mechanism rather than just asserting something is wrong.

The `--asan` check reads the log from `zeq2sanitize.sh` instead of re-running it
(that takes minutes). It reports INCONCLUSIVE rather than PASS if the run never
reached gameplay, since a clean log from a run that died during startup proves
nothing.

### What the strncpyz gate exists to catch

`Game/CGame/cg_weapGfxParser.c` used to size a copy into
`weaponName[MAX_WEAPONNAME]` (40 bytes) with `sizeof()` of a `MAX_QPATH` (64)
sibling field, and `weaponName` is the last member of its struct. `Q_strncpyz`
wraps `strncpy`, which pads the destination out to `n`, so that wrote 24 bytes
past the end of the object on **every** call regardless of input length, from
`CG_RegisterClients` during cgame init — i.e. whenever a client loaded.

Fixed, and the gate is green. It stays because the mistake is easy to
reintroduce: the sibling field is one identifier away and the code still
compiles.

## Typical loop

```bash
Tools/dev/zeq2build.sh cgame     # recompile cgame and stage it
Tools/dev/zeq2shot.sh --stats    # look at a settled in-game frame
Tools/dev/zeq2smoke.sh           # regression gate over every map
```

## Checking Linux (`zeq2linux.sh`)

```bash
Tools/dev/zeq2linux.sh build      # native arm64 Linux, about a minute
Tools/dev/zeq2linux.sh test       # lint + every suite under ASan/UBSan
Tools/dev/zeq2linux.sh --amd64 build   # exactly what CI runs; emulated, slow
```

First run builds a container from `Dockerfile.linux` and caches it; after that
the apt install is free. Outputs go to `Build/linux-<arch>/` and
`tests/bin-linux-<arch>/`, so nothing collides with the mac tree — the suites
are native binaries and running an x86_64 one under arm64 gets you a rosetta
error rather than a test failure.

**Run this before pushing anything that touches `Shared/` or `Game/`.** Two
things are invisible on darwin-arm:

- **The QVM bytecode is never compiled here.** arm64 has no bytecode JIT, so the
  Makefile sets `BUILD_GAME_QVM=0` and the whole q3lcc/q3asm toolchain is
  skipped. `Com_ScreenFovX` calling `atan()` therefore built fine on the mac and
  broke every QVM link, because the bytecode's maths are engine syscalls
  (`g_syscalls.asm`) and there is no `atan` trap — only `atan2`. The script
  passes `BUILD_GAME_QVM=1` on both arches so the toolchain always runs.
- **`ld64` tolerates undefined symbols that GNU `ld` rejects.** A test suite
  missing a stub for a function it never calls links on macOS and fails on
  Linux.

## Why the smoke test drives the client, not the dedicated server

The dedicated build excludes `cl_cgame.o`, so it never loads the cgame module.
Every join-time crash found in this codebase so far has been *in* cgame, so a
server-only smoke test would have passed while the game was unplayable. Use
`--dedicated` to add server coverage, not to replace client coverage.

## Four traps these scripts exist to avoid

**`+set` loses to the config for any archived cvar.** Startup execs
`default.cfg` and then `zeq2config.cfg`, and both are full of `seta` lines. A
`+set` on the command line is applied *before* that, so the config overwrites it
and the game runs with the value you thought you had replaced. This is silent:
nothing warns, and the cvar reads back as the config's value.

It applies to every `CVAR_ARCHIVE` cvar the saved config mentions —
`s_musicvolume`, `cg_thirdPersonSlide`, `r_picmip`, `model`, and most of the
rest. Cvars absent from both configs are unaffected, which is why
`+set s_initsound 0` sticks and `+set s_musicvolume 0` does not.

Two ways round it, in order of preference:

```bash
# a bare cvar name is a console command, so it goes into the command buffer
# and runs after the configs have exec'd - this wins
ZEQ2.arm ... +cg_thirdPersonSlide 0 +map desert

# or edit zeq2config.cfg, but the engine rewrites it at shutdown, so change it
# with the game closed or the change is lost
```

Check which you got: the value is echoed in the startup log for some cvars
(`picmip: 1`), and `+cvarlist <name>` or the console shows the rest. If a
`+set` appears to do nothing, this is why — reach for it before assuming the
feature is broken. `zeq2aura.sh` passes its camera cvars as bare commands for
exactly this reason.

**The other half of that: a bare cvar command *persists*.** Winning the fight
above means the cvar is genuinely set, and the engine writes every
`CVAR_ARCHIVE` cvar back to `zeq2config.cfg` at shutdown. So a screenshot run
rewrites the player's saved settings with whatever the harness wanted, and the
next time they start the game by hand there is no HUD and the camera sits in a
screenshot pose. Nothing about that points back at a dev script, which is what
makes it expensive.

`zeq2shot.sh` now backs up `zeq2config.cfg` before launching and restores it on
exit, so every script that goes through it is safe. **Anything that launches
the engine directly has to do the same.** Resetting the individual cvars
afterwards is not good enough - it needs updating every time a caller adds one.

The cvars the visual harness overrides, and the values the game ships with, so
a polluted config can be repaired by hand:

| cvar | harness uses | default | effect if it leaks |
|---|---|---|---|
| `cg_draw2D` | `0` | `1` | no HUD at all |
| `cg_thirdPersonRange` | `90`–`130` | `80` | camera too far back |
| `cg_thirdPersonSlide` | `0` | `-20` | player centred, not offset |
| `cg_thirdPersonHeight` | `0` | `0` | harmless, same as default |
| `cg_thirdPersonAngle` | `0` | `0` | not archived, never persists |
| `cg_auraScreenSpace` | `1` | `1` | harmless; `0` is the old hull path |
| `model` | per-test | `goku` | **worst case** - an unloadable model leaves the game sitting at the menu with the map loaded, and the logs read as a clean start |

**Module staging.** `make` writes `Build/<cfg>/Base/cgame$(uname -p).dylib`
(e.g. `cgamearm.dylib`), but the engine derives its module name from
`ARCH_STRING` in `Shared/q_platform.h`, which is `arm64`. It therefore loads
`ZEQ2/cgamearm64.dylib`. Skip the copy and you test a stale module and conclude,
wrongly, that your fix did nothing. `zeq2build.sh` always stages both names.

**Stale objects across checkouts.** The `.d` dependency files record absolute
paths. If the object files were produced in a different checkout (a clone, a
worktree), make believes they are current and will not rebuild. Pass a module
name — `zeq2build.sh cgame` — to drop its objects first.

## Reading a frame without a screen

`screencapture` needs macOS Screen Recording permission, which a headless shell
usually does not have. The engine's own `screenshot` command does not: it reads
the backbuffer from inside the render command queue. `zeq2shot.sh` drives that
via `+wait <frames> +screenshot`, so it captures a *settled* in-game frame
rather than a loading screen.

`--stats` prints a colour histogram, which distinguishes outcomes mechanically
instead of by eye:

- ~2.5k distinct colours, >90% greyscale → world is not drawing (flat frame)
- ~40k distinct colours, <10% greyscale → world is drawing

## Judging animation (`zeq2clip.sh`)

A screenshot cannot judge motion, and eyeballing two live windows cannot put
two shaders on the same motion. A demo can: `--record` scripts a session and
saves it under `demos/`, and `--play` replays it through the engine's `video`
capture - so N replays with N different `--vp`/`--fp` overlays differ by
exactly the shader. `.gif` output needs ffmpeg; without it the engine's
MJPEG-AVI is kept instead.

The overlays, the tier-config `--set`s and the camera cvars are all restored
after every run, so a clip session leaves the install as it found it.
`Tools/dev/aura_variants/` holds the aura's animation candidates as complete
shader pairs, with the loop that compares them in its README.

Two determinism caveats. A *recording* is only as repeatable as the session
that made it - spawn points vary per launch - so keep and reuse the recorded
demo rather than re-recording it, and validation against old clips holds only
while the demo file does. And `wait` counts client frames, so `--settle` and
`--frames` scale with the machine's frame rate, not wall time.

## Sanitizers: ASan and UBSan both work

`zeq2sanitize.sh` builds with `-fsanitize=address,undefined` and runs to a full
in-game frame.

ASan used to be unusable, and the reason is worth remembering: the build linked
homebrew's `sdl12-compat`, a shim that reaches SDL3 through `dlopen`
(engine -> sdl12-compat -> sdl2-compat -> SDL3). ASan's `malloc` interception
breaks that chain, so sdl12-compat printed `Failed loading SDL2 library` and
aborted before the engine produced any output. The engine now links **real
SDL2** directly, with no shim and no `dlopen`, so ASan starts normally. See the
`SDL2_PREFIX` comment in the `Makefile`'s darwin section — note especially that
`/opt/homebrew/opt/sdl2` is itself a symlink to `sdl2-compat`, so the real keg
has to be named by its versioned Cellar path, and the resulting linkage is
verified after every link because getting it wrong is otherwise silent.

Three settings the script relies on; don't drop them:

- `-fsanitize-recover=address` — without it ASan aborts on the first hard memory
  error. There is one during cgame init, so you would never reach gameplay and
  every later finding would stay invisible. `halt_on_error=0` does nothing for
  non-recoverable checks.
- `detect_odr_violation=0` — cgame/game/ui/renderer are separate dylibs that each
  statically link the same `Shared/*.c`, so shared globals legitimately exist
  several times over. Left on, ASan emits ~90 `odr-violation` reports *and*
  poisons one copy of each duplicated global, which then surfaces as bogus
  overflows in unrelated code.
- `detect_leaks=0` — the engine does not free at shutdown; not interesting here.

Reading the output:

- ASan findings are real memory errors, with one exception. Intermittent reports
  of a READ at *"0 bytes inside of global variable `.str.NN` defined in
  Game/UI/…"* — the first byte of a plainly in-bounds string literal — come from
  the engine dlclose'ing and re-dlopen'ing the VM dylibs (`CL_FlushMemory` ->
  `CL_StartHunkUsers`). ASan poisons an unloaded module's globals and does not
  reliably unpoison them when the image is remapped at the same address, so
  whether they appear depends on ASLR. If the `is located` line says the access
  sits inside a global large enough for it, ignore it.
- UBSan findings are a latent-UB inventory, not necessarily live bugs.
- About one run in four dies early with SIGSEGV or SIGTERM. Re-run;
  `reached in-game: yes` is what tells you the run was usable.

Two environment notes. ASan misbehaves under a sandboxed shell (`Checking file
existence is not allowed under sandbox`), so run it unsandboxed. And
`Sys_SigHandler` (`Engine/sys/sys_main.c`) installs a `SIGABRT` handler, so a
sanitizer abort surfaces only as `Sys_SigHandler: caught signal 6` with no
diagnostic — disable that handler if you need to debug an abort directly.

## Environment overrides

`ZEQ2_ROOT`, `ZEQ2_ARCH`, `ZEQ2_BUILD`, `ZEQ2_GAME` all have sensible defaults;
override them for a different build config or mod dir.
