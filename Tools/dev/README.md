# Tools/dev — deterministic dev loop for ZEQ2-Lite

Small, dependency-free scripts for the build → run → look-at-a-frame loop, so
that verifying a change never means re-deriving throwaway shell one-liners.
Python scripts use only the standard library; shell scripts need bash.

| script | what it does |
| --- | --- |
| `zeq2build.sh` | `make`, then stage the game modules where the engine loads them |
| `zeq2run.sh` | join a map, stay alive N seconds, report how the run ended |
| `zeq2shot.sh` | join a map, grab an in-engine screenshot, convert it to PNG |
| `zeq2bench.sh` | time the renderer on a scene that repeats exactly, and A/B a cvar |
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

## Benchmarking the renderer (`zeq2bench.sh`)

```bash
Tools/dev/zeq2bench.sh                    # 3 runs of the checked-in demo
Tools/dev/zeq2bench.sh --ab r_bloom 1 0   # A/B one cvar, arms interleaved
Tools/dev/zeq2bench.sh --record           # record a demo for a new branch
```

`--ab` is the one to reach for. It prints each run, then:

```
r_bloom 1: 12.45 ms (80.3 fps) over 4 runs, spread 30.7%
r_bloom 0: 5.40 ms (185.1 fps) over 4 runs, spread 13.0%
delta: -7.22 ms (-58.0%) per pair, ratio 0.42x (range 0.38-0.47)
```

Read the **ratio**, not the absolute milliseconds. This machine wanders by tens
of percent across a few minutes of sustained fullscreen load, which is the same
order as the effect most renderer changes have — so two separate invocations of
the script can easily disagree by more than the thing you are measuring. Two
runs a minute apart share that wander, so the pairwise ratio holds to a few
percent while the arms themselves do not. That is the whole reason `--ab`
interleaves instead of running one arm and then the other. A ratio range as wide
as the effect means the machine was too busy; run it again on a quiet one.

### Why a demo and not a live scene

Timedemo playback is the only scene in this game that repeats. The engine
replays recorded server snapshots and advances `cl.serverTime` by a fixed 50ms
per *rendered* frame, so frame N draws the same thing on any machine at any
speed, and the run reports the same frame count every time (1139 for the
checked-in `bench` demo — if that number moves, something ate part of the demo).
Live play does not repeat: two AI fighters diverge within a second and measure
±30%, and even an idle player drifts ±5% because the spawn point and view angle
are whatever the map's spawn logic picked.

`--record` produces a fresh demo. It uses `devmap` and `setviewpos` to pin the
viewpoint rather than accepting the spawn, so re-recording gives the same scene:

```bash
Tools/dev/zeq2bench.sh --record --map desert --viewpos "-32400 -23215 -4463 90"
cp Build/Release-darwin-arm/ZEQ2/demos/bench.dm_71 GameData/demos/
```

`GameData/` is the tracked overlay `zeq2build.sh` stages into the install, which
is how the demo reaches a fresh worktree — the install itself is not in the repo.

**A demo belongs to one protocol.** The file extension is the protocol version
(`Shared/qcommon.h`), so `bench.dm_71` is a master demo and a branch that moves
`PROTOCOL_VERSION` — `combat-and-ai` is at 72 — needs its own `--record`. A
build that cannot find a demo for its protocol prints `Not found: demos/…` and
drops to the main menu.

### Two ways to get a meaningless number

**Windowed runs measure the compositor.** A windowed present goes through the
macOS compositor, which puts a floor of roughly 4.9ms on the swap. That is a
third of a frame at the rates here, and it is not in the renderer. This script
overrides the windowed defaults in `zeq2env.sh` and warns if you force
`ZEQ2_FULLSCREEN=0`; those numbers are not comparable to fullscreen ones.

**Archived cvars leak between runs.** Nearly every `r_*` cvar is `CVAR_ARCHIVE`,
so `+set r_bloom 0` for one arm is written into `zeq2config.cfg` at shutdown and
silently becomes the default for every run after it — the B measurement gets
repeated under the name of A, and a whole batch of numbers is invalid with
nothing to show for it. The script restores the config *between* runs, not just
at the end, and pins every cvar that costs frame time at its shipped default on
every run, so a config some earlier session already polluted cannot move the
baseline. Restoring between runs is not only tidiness: leaving run 1's config in
place made run 2 abort playback with `Com_Error(code=3): Disconnected from
server`.

### `timedemo`, not `cl_timedemo`

`cl_timedemo` is the C variable; the cvar it holds is named **`timedemo`**
(`Engine/client/cl_main.c`, `Cvar_Get("timedemo", …)`). `+set cl_timedemo 1`
therefore creates an unrelated cvar, playback runs at wall-clock speed and
`CL_DemoCompleted` skips its `%i frames %3.1f seconds %3.1f fps` line entirely.

The other half of the same confusion: **a demo that finishes disconnects to the
main menu.** That is `CL_DemoCompleted` calling `CL_Disconnect`, i.e. success.
With a short demo it happens within a second or two of the map appearing, which
looks exactly like a demo that refused to play. Pass `+set nextdemo quit` so the
engine exits instead of sitting there.

Two more things that make a run vanish before it renders:

- `Com_ParseCommandLine` keeps only `MAX_CONSOLE_LINES` (32) `+` arguments and
  drops the rest **silently**. Push past it and the trailing `+demo` disappears,
  the engine starts, loads nothing and sits at the menu. This is why the pinned
  cvars go into an exec'd cfg instead of onto the command line.
- Escape on the connect screen issues `disconnect` (`Game/UI/ui_connect.c`), and
  `CL_InitCGame` spends several seconds not pumping the event queue, so events
  from the fullscreen handoff between back-to-back runs can land there. It shows
  up as `Com_Error(code=3): Disconnected from server` right after
  `CL_InitCGame`. The script settles between runs and retries a lost one; the
  scene is deterministic, so a replayed run measures the same frames.

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
exit, so every script that goes through it is safe; `zeq2bench.sh` launches
directly and does the same, between every run. **Anything that launches the
engine directly has to do the same.** Resetting the individual cvars afterwards
is not good enough - it needs updating every time a caller adds one.

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
