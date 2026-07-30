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
| `zeq2sanitize.sh` | build + run under ASan/UBSan and group the findings |
| `zeq2test.sh` | **gate**: static checks, ASan demonstrations, sanitizer-log assertions |
| `tga2png.py` | convert an ioquake3 TGA screenshot to PNG (+ colour histogram) |
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

### Currently failing (by design — the defect is real and unfixed)

`Game/CGame/cg_weapGfxParser.c:2004,2006` size a copy into
`weaponName[MAX_WEAPONNAME]` (40 bytes) using
`sizeof(...missileTrailSpiralShader)` (`MAX_QPATH`, 64), and `weaponName` is the
last member of its struct. Because `Q_strncpyz` wraps `strncpy`, which pads the
destination out to `n` bytes, this writes 63 bytes plus a terminator on **every**
call regardless of input length — 24 bytes past the end of the object. It is
reached from `CG_RegisterClients` during cgame init, so it fires whenever a
client loads. Three sibling call sites name the wrong field too but are equal in
size today, so they are latent rather than live.

## Typical loop

```bash
Tools/dev/zeq2build.sh cgame     # recompile cgame and stage it
Tools/dev/zeq2shot.sh --stats    # look at a settled in-game frame
Tools/dev/zeq2smoke.sh           # regression gate over every map
```

## Why the smoke test drives the client, not the dedicated server

The dedicated build excludes `cl_cgame.o`, so it never loads the cgame module.
Every join-time crash found in this codebase so far has been *in* cgame, so a
server-only smoke test would have passed while the game was unplayable. Use
`--dedicated` to add server coverage, not to replace client coverage.

## Two traps these scripts exist to avoid

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
