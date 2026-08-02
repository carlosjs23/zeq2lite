# Tools/dev — deterministic dev loop for ZEQ2-Lite

Small, dependency-free scripts for the build → run → look-at-a-frame loop, so
that verifying a change never means re-deriving throwaway shell one-liners.
Python scripts use only the standard library; shell scripts need bash.

| script | what it does |
| --- | --- |
| `zeq2build.sh` | `make`, then stage the game modules where the engine loads them |
| `zeq2run.sh` | join a map, stay alive N seconds, report how the run ended |
| `zeq2shot.sh` | join a map, grab an in-engine screenshot, convert it to PNG |
| `zeq2duel.sh` | fight two AI opponents and report what each spent doing it; `--assert` makes it a gate |
| `zeq2smoke.sh` | **gate**: load every map, assert the game survives joining it |
| `zeq2audit.sh` | report where the code expects assets the data set never shipped |
| `zeq2aura.sh` | sweep the aura's tuning and contact-sheet what each value renders as |
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

## Looking at a fight

`zeq2duel.sh` puts two AI opponents in a map, lets them fight, and reads the
result out of `g_debugFight` — one line per fighter per sample carrying the
things a fight is actually made of and none of which are on screen as numbers:
the guard behind the power bar, the two pools, the lock, the buttons held, and
whether each defensive verb is in use.

```bash
Tools/dev/zeq2duel.sh --seconds 120       # long enough for guards to break
```

The summary is per fighter: what it opened and closed with, its guard's
low-water mark, and how many samples it spent blocking, teleporting, boosting
or struggling. **A verb with a count of zero is one the fight never had a
reason to use**, which is the difference between a mechanic and a gimmick, and
it is the signal to read after any balance change.

The cvar works on its own for a fight involving a human: `g_debugFight 1000`
reports every client every second, including yours.

Counts are events, not samples. Every verb, the death count and the guard's
low-water mark are tracked per frame in `G_DebugFight` and read off the last
line, because anything sampled at the reporting interval measures *how long*
something lasted rather than *how often* it happened. A zanzoken is up for a
few hundred milliseconds; the death count once reported ten for a duel with
two obituaries.

### The charge funnel

`charged` / `ready` / `fired` are three stages of one thing, and the split is
what makes a stalled fight diagnosable:

- **charged** — windups begun.
- **ready** — windups that reached `chargeReadyPct`. Below it, any interrupt
  discards the charge outright, so this is the stage that usually leaks.
- **fired** — windups that became a shot.

`charges lost to:` names what ended each windup that never fired — `melee`,
`knock`, `trans`, `soar`, `died`, or `other` when nothing external was up and
the fighter simply let go of the button. An aggregate "began 90, fired 6"
supports two opposite explanations; these separate them in one run.

### As a gate (`--assert`)

```bash
Tools/dev/zeq2duel.sh --seconds 240 --skill 5 --assert
```

Exits non-zero when the fight has stopped working: fewer fighters than
expected, no melee exchange opened, or a charge-to-ready ratio under the floor.
These are not balance targets - they are the shape of a fight that has
degenerated, which a code change can cause silently and which no unit test can
see.

**Use a long run.** The metrics are noisy over 60s - consecutive runs have given
ready ratios of 0.075 and 0.348 - so a short duel will flap. 240s is the
shortest that has been stable.

### Watching it live (`cg_debugFight`)

```
cg_debugFight 1
```

Draws the followed fighter's decision inputs on screen: pools, weapon state,
charge percent and whether it is ready, lock and distance, melee state, and the
timers every melee branch refuses on. The duel harness leaves you spectating and
following, so this shows the AI's state as it fights. Cheat-gated and not
archived.

It reads `cg.snap->ps`, so it shows what the *client* knows. The AI's own
intent - its plant range, its held rolls, its tendency counters - stays
server-side on the fight line, because putting it on screen would mean
networking it.

## Why the smoke test drives the client, not the dedicated server

The dedicated build excludes `cl_cgame.o`, so it never loads the cgame module.
Every join-time crash found in this codebase so far has been *in* cgame, so a
server-only smoke test would have passed while the game was unplayable. Use
`--dedicated` to add server coverage, not to replace client coverage.

## Four traps these scripts exist to avoid

**`+set` beats the config; code that assigns the cvar later beats `+set`.**
Startup execs `default.cfg` and then `zeq2config.cfg`, both full of `seta`
lines — but `Com_StartupVariable( NULL )` runs *after* that pass, in
`Shared/common.c` directly beneath the comment "override anything from the
config files with command line args". So `+set r_picmip 3` and
`+set cg_thirdPersonSlide 7` both win over the saved config, and both are
written back at shutdown. Verified against the source and by launching.

What does silently eat a `+set` is a cvar something else keeps assigning.
`s_musicvolume` is the case that taught the wrong lesson here: `cg_music.c`
re-derives it from `cg_music` every time a track starts, so neither
`+set s_musicvolume 0.5` nor a bare `+s_musicvolume 0.5` survives the first
track change — set `cg_music` instead. When a cvar refuses to hold a value,
grep for who else writes it before blaming the config.

A bare cvar name is a console command rather than a startup variable, so it
lands in the command buffer and runs later still. That ordering rarely matters
now, but it costs nothing:

```bash
ZEQ2.arm ... +cg_thirdPersonSlide 0 +map desert
```

Either form is echoed in the startup log for some cvars (`picmip: 1`);
`+cvarlist <name>` or the console covers the rest.

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
- `reached in-game: yes` is what tells you the run was usable — a run that stops
  short covers only loading, so the absence of a finding means nothing.

Two environment notes. ASan misbehaves under a sandboxed shell (`Checking file
existence is not allowed under sandbox`), so run it unsandboxed. And
`Sys_SigHandler` catches the fault signals, so a sanitizer abort surfaces only
as `=== fatal signal, exiting ===` with no diagnostic — set
`ZEQ2_NO_SIGHANDLER=1` to hand the fault straight to the sanitizer. The
termination signals — `SIGTERM`, `SIGINT`, `SIGHUP` — stay handled with it set,
so the orderly shutdown is still under test.

## Environment overrides

`ZEQ2_ROOT`, `ZEQ2_ARCH`, `ZEQ2_BUILD`, `ZEQ2_GAME` all have sensible defaults;
override them for a different build config or mod dir.
