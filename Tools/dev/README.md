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
| `zeq2duel.sh` | fight two AI opponents and report what each spent doing it; `--assert` makes it a gate |
| `zeq2smoke.sh` | **gate**: load every map, assert the game survives joining it |
| `zeq2audit.sh` | report where the code expects assets the data set never shipped |
| `zeq2aura.sh` | sweep the aura's tuning and contact-sheet what each value renders as |
| `zeq2clip.sh` | record a demo once, replay it through shader variants as video clips |
| `zeq2sanitize.sh` | build + run under ASan/UBSan and group the findings |
| `zeq2test.sh` | **gate**: static checks, ASan demonstrations, sanitizer-log assertions |
| `tga2png.py` | convert an ioquake3 TGA screenshot to PNG (+ colour histogram) |
| `png_sheet.py` | flatten PNGs onto one background so transparent art can be looked at |
| `make_ui_art.py` | generate the interface images the data set never shipped |
| `make_ring_art.py` | generate the tournament ring's floor, kerb, posts and ki wall |
| `make_aura_mesh.py` | generate the screen-space aura's ring mesh |
| `iqm.py` | write IQM models this engine's loader accepts (library, not a script) |
| `md3_to_iqm.py` | convert a character to skeletal IQM, rigid or skinned; `--report` measures what that costs |
| `ssdr.py` | solve a skeleton and skin weights from vertex-animated geometry (**needs numpy**) |
| `auragen.c` | procedurally generate aura reference images (compile: `cc -O2 -o auragen auragen.c -lz`) |
| `aura_reference_clean.py` | turn any aura art into pipeline form - strips painted checkerboards (**needs numpy+scipy**) |
| `make_aura_texture.py` | generate the screen-space aura's spike strip |
| `zeq2env.sh` | shared paths/helpers, sourced by the others (not run directly) |

`zeq2smoke.sh` and `zeq2test.sh` are gates (non-zero exit on failure). The rest
are reports.

The screen-space aura's whole pipeline - one reference image to the 1:1
in-game effect, its measurement loop and its animation A/B - is documented in
`AURA.md`.

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

## Holding an input from a script (`testinput`)

Three of this game's mechanics are only reachable by *holding* a key: staying
airborne, keeping the melee lock button down, and charging an attack. A usercmd
is built from key state in `CL_CreateCmd`, so `+moveup` issued as a script line
has no key behind it and nothing downstream can tell a held button from a
one-frame press. `testinput` folds the hold in where every other input source
reaches the usercmd, so prediction, networking and pmove see exactly what a
human press produces.

```
testinput <name>[,<name>...] <ms>
testinput clear
```

Names: `jump attack altattack lock block boost powerup up down forward back
left right`. `lock` is the melee lock-on — `BUTTON_GESTURE`, which `default.cfg`
binds to `g` as `+button3`.

**The separator is a comma, not a plus.** `Com_ParseCommandLine` breaks the
command line on every `+`, so `+testinput lock+altattack 8000` would arrive as
two console lines.

A second invocation replaces the first outright, `testinput clear` stops
everything, and the hold expires on its own after `<ms>`. While nothing is held
the usercmd path is byte for byte what it was before.

**Gated** on `cl_connectedToCheatServer` — the flag the client already uses to
decide whether `CVAR_CHEAT` cvars may keep their values — or on `com_developer`
for a run that never connects to a cheat server. There is no client-side
equivalent of the game module's `g_cheats.integer` test, so that is the nearest
idiom the engine has.

Two things worth knowing before writing a run:

- **`jump` alone does not lift you.** `BUTTON_JUMP` starts a jump from the
  ground; sustained flight is `upmove`. Use `jump,up`.
- **`lock` is a toggle**, not a state. Holding it acquires on the first frame
  and `PMF_LOCK_HELD` swallows the rest, so `lock,altattack` for the whole
  window is right and re-issuing `lock` would drop the target.

```bash
# 45 seconds airborne - the objective that could not be scripted before
testinput jump,up 90000

# lock onto the nearest target and charge, both held at once
dummy goku 200
testinput lock,altattack 12000
```

Read the result out of `ruledump` (objective, PERS progress, tags), `games.log`
(`Training: … objective-complete <id>`) or `g_debugFight` (`lock`, `buttons`,
`charge`).

Still not scriptable through `testinput`: **Escape**. It is handled in engine
key handling (`CL_KeyEvent` and the key catcher) rather than in the usercmd, so
menus and the journal page still need a person, a `bind`-driven command, or
`testkey` below.

## Typing from a script (`testkey`)

`testinput` reaches the usercmd; `testkey` reaches the *key catcher*, which is
where the console, the chat line and the menus read input. It queues events
through `Com_QueueEvent` exactly as `sdl_input.c` does, so they take the same
path a real press takes.

```
testkey text <string>          # one SE_CHAR per byte, no spaces (argv splits)
testkey char <code>            # one SE_CHAR - 22 is ctrl-v, 3 ctrl-c, 24 ctrl-x
testkey key <keynum> <0|1>     # SE_KEY down/up, keynums from keycodes.h
```

Gated like `testinput`. This is what makes the clipboard testable end to end:

```bash
# paste - the console edit line should show what pbcopy put there
printf 'testpaste123' | pbcopy
zeq2shot.sh --after toggleconsole --after "testkey char 22" --out /tmp/p.png

# copy - assert from the shell, no screenshot needed
zeq2shot.sh --after toggleconsole --after "testkey text hello" \
            --after "testkey char 3" --out /tmp/c.png
pbpaste          # -> hello
```

What it cannot reach is the SDL layer itself: `testkey` starts *after*
translation, so the cmd-c/v/x folding in `sdl_input.c` has to be tried by hand.

## Practising the Budokai against a bot

An `ai` fighter is a full tournament participant: it takes a place on the fight
line, `CalculateRanks` counts it, and losing puts it at the back of the queue
the way losing puts a human there. Two things about the setup are not obvious:

- **`g_warmup 1` starts the round without a `map_restart`.** `CheckTournament`
  sets `warmupTime` straight to zero when `g_warmup` is not above one, which is
  what lets a whole run be scripted from the startup command buffer — a queued
  `map_restart` cannot execute until that buffer drains, so anything after a
  `wait` would otherwise run in the wrong level.
- **Both fighters spawn in the ring**, not at the map's spawn points, which on
  every shipped map are nowhere near the authored arena. Without that a round
  ends by ring-out on its first frame.

```bash
Tools/dev/zeq2shot.sh --map desert --frames 700 \
  --after "ai goku 300" --after "wait 900" \
  --after "setviewpos -39730 4681 1500 180" --after "wait 2000" \
  -- +set g_gametype 1 +set g_training 1 +set g_warmup 1
```

That coordinate is a hundred units past the desert ring's edge and a thousand
above the terrain there, so the second `wait` is the fall: ring-out needs the
loser to *touch down*, and flying out over the edge is legal. To see the next
round as well, launch the engine directly and let the buffer drain instead —
the `map_restart` the round end queues cannot run while a `wait` is holding it.

Read the round out of `games.log`: `Budokai: roundState 2` is a round being
fought, `Budokai: ringout <loser> winner <winner>` and `Exit: Ring out.` are how
one ends, and the `InitGame` block after it is the next round with both fighters
back.

## `setviewpos` and the "set" prefix

`+setviewpos x y z yaw` on the command line used to do nothing at all, silently:
no cheat refusal, no usage line, no `unknown cmd`. `Com_AddStartupCommands`
skipped every startup line matching `Q_stricmpn( line, "set", 3 )` on the
grounds that `Com_StartupVariable` had already applied it — but that function
consumes exactly `set`, so the three-character prefix test also ate
`setviewpos` (and `seta`, `sets`, `setu`) before it ever reached the command
buffer. It now compares the whole command name, so `+setviewpos` behaves like
the same line typed at the console.

`Cmd_SetViewpos_f` also clears the velocity and the `PMF_TIME_KNOCKBACK` hold
that `TeleportPlayer` applies. A teleporter is meant to spit the player out
along its exit angles; a debug pin is not, and a scripted run asking for a
coordinate has to land on it.

Check arrival with `masterlist` (`… radius 768, 32 away`) or `where`.

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

**A killed run blocks the next launch.** `Com_Init` writes a pid file and only
`Sys_Exit` removes it, so a run killed at a deadline — which is what every
script here does — leaves a pid that is not running. The next launch reads it,
decides the last session crashed and opens the modal "Abnormal Exit" NSAlert,
which nobody is there to answer: the run hangs forever with its log stopping
right after `Hunk_Clear`. `zeq2_require_bin` now clears a stale one, so every
launcher gets the guard.

The path is not the obvious one. `Sys_TempPath` is
`FSFindFolder( kTemporaryFolderType )` on macOS, which resolves to
`$TMPDIR/TemporaryItems`, *not* `$TMPDIR` — and that directory is mode 700 and
cannot be listed from a sandboxed shell even though a file in it can be read and
removed by name. `getconf DARWIN_USER_TEMP_DIR` reaches the same per-user
domain, so the path is derived rather than hardcoded; a `/var/folders/...`
literal would be wrong on any other account. A pid that is still running is left
alone, because a concurrent session writes the same file.

**Stale objects across checkouts.** The `.d` dependency files record absolute
paths. If the object files were produced in a different checkout (a clone, a
worktree), make believes they are current and will not rebuild. Pass a module
name — `zeq2build.sh cgame` — to drop its objects first.

## Where games.log actually lives, and why it goes quiet

There is exactly one `games.log`, at **`$ZEQ2_BUILD/$ZEQ2_GAME/games.log`** —
`Build/Release-darwin-arm/ZEQ2/games.log` on this machine. There is no second
copy under the home path to go looking for: this fork gives `fs_homepath` the
same default as `fs_basepath` (`Sys_DefaultInstallPath()`, `Shared/files.c`), so
`trap_FS_FOpenFileByMode( …, FS_APPEND )` resolves to the install directory and
`FS_Startup` prints a single search path to prove it.

What makes the file look like it went stale is `g_log`, and it is worth reading
the mechanism once because nothing about the symptom points at it:

- The shipped `ZEQ2/default.cfg` contains `seta g_log ""`, and `ZEQ2_server.cfg`
  repeats it. That exec runs long before the game module loads.
- `trap_Cvar_Register( … "g_log", "games.log", CVAR_ARCHIVE )` reaches
  `Cvar_Get`, which for a cvar that already exists **keeps the existing value**
  and only adopts `"games.log"` as the *reset* string. So `g_log` stays empty.
- `G_InitGame` therefore never opens the file. It says so —
  `Not logging to disk: g_log is ""` — but that is one line in a very long
  startup log.
- `g_log` is `CVAR_ARCHIVE`, so whichever value a session ends with is written
  into `zeq2config.cfg` and inherited by the next one. A run launched with
  `+set g_log games.log` leaves `seta g_log "games.log"` behind and every later
  run logs; a script that backs up `zeq2config.cfg` before that run and restores
  it afterwards — which `zeq2shot.sh`, `zeq2duel.sh`, `zeq2clip.sh` and
  `zeq2bench.sh` all correctly do — puts the empty value back, and the log stops
  without a word. That is the "it worked this morning" case exactly.

`zeq2_base_args` now states `+set g_log games.log` on every launch, so no
scripted run depends on what the saved config happens to hold. Override with
`ZEQ2_LOG=""` to turn logging off for a run. A run launched by hand still needs
the `+set`, or it inherits the empty value from the config.

Two consecutive script-launched runs must therefore *append* to the same file:
the mtime and the line count both advance, and each run adds one `InitGame`
block per level load — plus one more for every `map_restart`, which in
`GT_TOURNAMENT` is once per round.

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
- `reached in-game: yes` is what tells you the run was usable — a run that stops
  short covers only loading, so the absence of a finding means nothing.

Two environment notes. ASan misbehaves under a sandboxed shell (`Checking file
existence is not allowed under sandbox`), so run it unsandboxed. And
`Sys_SigHandler` catches the fault signals, so a sanitizer abort surfaces only
as `=== fatal signal, exiting ===` with no diagnostic — set
`ZEQ2_NO_SIGHANDLER=1` to hand the fault straight to the sanitizer. The
termination signals — `SIGTERM`, `SIGINT`, `SIGHUP` — stay handled with it set,
so the orderly shutdown is still under test.

## The masters' heads (`md3.py`, `make_headgear.py`, `make_master_faces.py`)

Not part of `zeq2build.sh`. These need Pillow and numpy, and `zeq2build.sh`
runs the generators under bare `python3`, which has neither — so they are run
by hand against an install, from a venv:

```bash
python3 Tools/dev/make_headgear.py     Build/Release-darwin-arm/ZEQ2/players \
                                       Build/Release-darwin-arm/ZEQ2/scripts
python3 Tools/dev/make_master_faces.py Build/Release-darwin-arm/ZEQ2/players
```

Both are idempotent and re-runnable: the head is rebuilt from the donor every
time and the skins are re-keyed rather than appended to, so running twice is
the same as running once. Every file they replace is kept beside it —
`head.md3.predonor`, `*.skin.preheadgear`.

Two things they exist to stop happening again:

**A repaint done on the flat sheet is a repaint done blind.** The donor heads
unwrap radially, so the top of the sheet is the crown; a "headband" painted as
a horizontal stripe lands on the top of the skull and wraps as a cap. Paint in
head coordinates instead — `make_master_faces.py` rasterises the UV triangles
into a position map and places every mark by where it is on the head — and use
`uv_report.py` to see the layout before touching a sheet.

**A donor head only fits a body whose `tag_head` was authored for it.**
`CG_PositionRotatedEntityOnTag` hands the head that tag's axes. nappa's body
hands (0,1,0) as forward and krillin-derived bodies hand (-0.46,0,-0.89), so
nappa's head on those bodies is on backwards, and because the tags are
per-frame there is no single rotation that fixes it. Check a donor against the
body before kitbashing, and prefer the donor the body already agrees with.

`md3_preview.py` renders a model and sheet offline, which is how to iterate on
either of these without a map load per attempt.

## Skeletal characters (`iqm.py`, `md3_to_iqm.py`)

`zeq2build.sh` converts the five training masters to `players/<who>/tier1/character.iqm`
on every build, the same way it generates the aura mesh: the converter is the
source, the `.iqm` is a build product, and nothing binary is committed.
`cg_master.c` draws the IQM when it registered and the three md3s when it did
not, so deleting a `character.iqm` is a complete rollback.

Three things about this engine's IQM path decide the shape of everything the
writer emits, and none of them is in the IQM specification:

**A joint is only usable as a tag if its bind transform is the identity.**
`R_IQMLerpTag` returns `ComputeJointMats`' output, which is
`pose_global * inverse(bind_global)` - a skinning matrix. That equals the
joint's own transform only when the bind is the identity. So `iqm.py` binds
every joint at the identity and puts the entire rest pose in the pose track,
which also means mesh vertices are authored in plain model space. Bind a joint
at its rest transform instead and gear attached to it sits at an offset with
nothing in any log to explain it.

**`num_poses` must equal `num_joints`,** or the loader rejects the file with
nothing but "couldn't load iqm file".

**The vertex path is skinning-only.** A vertex whose blend weights sum to zero
lands on the origin. The model loads, draws, and is invisible.

`tests/suites/test_iqm.c` pins all three.

### The two binds, and what each one costs

md3 is vertex animation and a skeleton cannot represent that in general. There
are two ways out of it here and they measure two orders of magnitude apart.

**The rigid bind** (`md3_to_iqm.py <dir> <out.iqm>`) makes lower, upper and
head one bone each, animated by the md3's own tag chain, and every md3 tag a
bone of its own so `trap_R_LerpTag` keeps working. This is what a master is
drawn from: one entity, one skeleton, `character.iqm`. What it loses is
deformation *inside* a part - striding legs, swinging arms.

**The decomposition** (`md3_to_iqm.py <dir> --parts`) solves the bones out of
the vertex trajectories instead. `ssdr.py` implements Smooth Skinning
Decomposition with Rigid Bones: cluster the vertices by whose rigid motion
explains them, then alternate between fitting each bone's per-frame transform
and fitting each vertex's four blend weights, until the reconstruction stops
improving. It writes `lower.iqm`, `upper.iqm` and `head.iqm`, each a drop-in
for its md3.

**Per part, not per character, and that is not a detail.**
`CG_PlayerAnimation` hands legs, torso and head their own frame numbers, which
is how a fighter runs with his legs while his torso throws a punch. One
skeleton over the whole character has one frame index and cannot do that. Three
skeletons joined by the same tag chain can, and leave the .skin files, the
damage states, the weapon tag, the aura tags and the melee anchor untouched.

`--report` measures both binds in world units against a fighter about 56 units
tall:

```
goku  668 frames, reference frame 107, bones lower=16 upper=24 head=12
  lower, 790 verts
    class      frames    rigid decomposed
    all           668    11.55       0.03
    melee         128    15.15       0.03
```

The rigid residual is eight to thirteen units on every character - a fifth of
body height, a statue sliding around the map. The decomposed residual is a
tenth of a unit, which is the same order as the *idle*'s rigid residual - the
number that made the masters convert with no visible loss. Every character in
the roster clears it, on every animation class:

| character | rigid, all | decomposed, all | worst class (decomposed) |
| --- | --- | --- | --- |
| goku | 9.95 | 0.06 | 0.08 swim |
| krillin | 8.62 | 0.08 | 0.12 run |
| piccolo | 12.20 | 0.13 | 0.20 swim |
| frieza | 8.00 | 0.14 | 0.17 jump |
| nappa | 13.18 | 0.16 | 0.34 swim |
| raditz | 10.52 | 0.13 | 0.18 kiattack |
| vegetaCell | 9.21 | 0.07 | 0.12 kiattack |
| vegetaSaiyan | 9.63 | 0.17 | 0.21 kiattack |

The ten non-playable dirs measure the same way; several are kitbashes of these
models and report identical numbers, which is a useful check that the solve is
deterministic. **The whole roster is converted.** Nothing stays on md3 for
accuracy reasons, and a character with no `.iqm` still draws from its md3s down
the same path in `cg_tiers.c`, so deleting the files is a complete rollback.

### Choosing a bone count

`--sweep 4,8,12,16,24,32` prints the tradeoff. It is not the same for the three
parts, which is why `--bones` takes `lower=16,upper=24,head=12` as well as a
single number:

| bones | lower | upper | head |
| --- | --- | --- | --- |
| rigid | 11.55 | 11.43 | 0.39 |
| 4 | 1.07 | 0.96 | 0.07 |
| 8 | 0.15 | 0.53 | 0.04 |
| 12 | 0.04 | 0.30 | 0.03 |
| 16 | 0.03 | 0.17 | 0.02 |
| 24 | 0.02 | 0.08 | 0.07 |
| 32 | 0.02 | 0.05 | 0.02 |

The legs are a few rigid segments and saturate by twelve. The torso carries two
arms and keeps improving past twenty-four. The head barely deforms at all - and
note that it gets *worse* at 24 than at 16: the clustering is initialised by
k-means and more bones than the motion needs give it more ways to land in a
poor local minimum. More bones is not monotonically better, so read the sweep
rather than assuming.

### Running the conversion

`zeq2build.sh` does it, and skips a character whose three `.iqm` files are
newer than its md3s and than `ssdr.py`, `md3_to_iqm.py`, `iqm.py` and `md3.py`.
A first build is about a minute per character; every build after that is free.
Touching the solver invalidates all of them, which is intended - a rig that no
longer matches the tool that made it is the failure this rule exists to stop.

**Needs numpy**, and only that: no scipy, no other package. Without numpy the
build prints a note and the roster stays on md3, which is a working game rather
than a broken one. `python3 Tools/dev/ssdr.py --self-test` checks the solver's
pieces against synthetic rigs with known answers and takes a second.

### What it costs to draw

IQM skinning is CPU work per vertex per frame - `RB_IQMSurfaceAnim` blends four
matrices and transforms a position and a normal for each of about 3000 vertices
per character - where md3 lerps two frames of positions. Measured on the
`abtest` demo, one character on screen, arms interleaved to cancel thermal
drift:

```
IQM  3.32 ms   3.28 ms
md3  2.84 ms   2.81 ms
```

Half a millisecond per character per frame, repeatable to a hundredth. On a
16.7 ms budget that is three percent of a frame for one fighter and six for a
duel, which is the price of the roster deforming at all. If a crowded scene
ever needs it back, the lever is the bone count: `--sweep` shows the torso at
twelve bones is still forty times better than the rigid bind.

### Reading the solved skeleton

The bones are discovered, not authored, so their names say where on the body
the bone's rest centroid sits and nothing more: a vertical band (`foot`,
`shin`, `thigh`, `hips`, `chest`, `neck`, `head`), promoted to `arm` or `hand`
when the centroid is far enough off the body's mid-line, suffixed `_l` or `_r`,
and numbered from the top down when several land in the same band. The
hierarchy is a minimum spanning tree over "how much does bone B's origin wander
in bone A's frame", rooted at the bone nearest the pelvis.

Nothing in the fit depends on that tree. The bind is the identity and poses are
written relative to the parent, so any tree reproduces the same vertices
exactly; the tree exists because the renderer wants one and because
`refEntity_t`'s name-keyed bone overrides want names that mean something.

### Driving a bone from the game module

`refEntity_t` carries `bones[REF_MAX_BONES]`, each a joint name plus an additive
rotation and a per-axis scale (`Engine/renderer/tr_types.h`). Both are applied in
the bone's own frame, after the animated pose and before the bone is
concatenated onto its parent, so a rotation pivots at the joint and a scale
grows the bone's own geometry. Only MOD_IQM reads them; md3 entities are
untouched. A zeroed `refEntity_t` is a no-op - a zero scale reads as 1.

Two things to know before using them:

**Scale is inherited, and non-uniform scale is inherited as a squash of the
whole space below it.** Halving the `lower` bone's z does not just shorten the
legs, it flattens the torso and head that hang off it. A real proportion
control has to put the compensating inverse scale on the children.

**`R_LerpTag` gets no entity,** because `refexport_t` does not pass one. A tag
therefore resolves against the animated pose and ignores bone overrides, so gear
that has to follow a driven bone must be a mesh weighted to that bone - which is
how `md3_to_iqm.py` emits headgear - rather than a separate model on a tag.

The angles land in the bone's frame, which on the goku-derived rigs is the
model's frame: `tag_head`'s axes come back within half a degree of the identity.
A rig whose head tag carries a twist - the ones the donor-head note below is
about - would need that twist taken out first.

`cg_master.c` uses both: a master turns his head to follow the player and only
swings his body when the neck runs out of travel at
`CG_MASTER_NECK_YAW`, and `cg_masterProportion` scales the head and legs of the
copy `cg_masterCompare` draws.

### Looking at one

`cg_masterCompare <units>` (cheat) draws a converted master's md3 assembly that
far to his side, so both builds are in one frame under the same light. The md3
set is registered on demand the first time it is asked for, so a converted
master costs nothing until then:

```bash
Tools/dev/zeq2shot.sh --map desert --frames 900 \
  --after "cg_masterCompare 64" --after "cg_draw2D 0" \
  --after "setviewpos -40590 4560 1495 200" --out /tmp/side.png
```

`cg_masterProportion <headScale>` makes that copy a second skeletal draw with
its head scaled and its legs shortened to match, instead of the md3 assembly.
`cg_masterAnim <animNumber_t>` loops a different animation.cfg row - `9` is
ANIM_RUN, and running it with `cg_masterCompare` set is the one-frame version of
the residual table above: the md3 strides and the skeletal build stands still
with its torso rotating.

## Environment overrides

`ZEQ2_ROOT`, `ZEQ2_ARCH`, `ZEQ2_BUILD`, `ZEQ2_GAME` all have sensible defaults;
override them for a different build config or mod dir.
