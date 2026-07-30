# Unit tests

Criterion-based unit tests for the engine and game modules.

```bash
make test              # from the repo root
make -C tests          # same thing
make -C tests msg      # one suite
make -C tests list     # every registered test name
make -C tests coverage  # line coverage per file
make -C tests report    # JUnit XML for CI, into tests/bin/report/
```

Sanitizers are on by default (`SAN=0` to disable). ASan and UBSan cost nothing
at this scale and this codebase has live memory bugs, so tests keep them armed.

## Why Criterion

Each test runs **in its own process**. In a codebase we are actively de-bugging
for memory corruption that matters more than it sounds: a segfault or a
sanitizer abort fails exactly one test and is reported as a crash, instead of
taking down the whole run and hiding every test after it. It also means suites
can drive the big cgame globals (`cg`, `cgs`) directly without leaking state
between tests.

Tests self-register, so there is no runner list to maintain, and Criterion emits
JUnit XML natively for CI.

## Layout

```
tests/
  Makefile             one binary per suite, each linking only what it needs
  suites/test_*.c      the tests
  support/             link seams: stubs and fakes
  lint/                source-level checks (see below)
```

## Adding a suite

1. Write `suites/test_<name>.c`:

   ```c
   #include <criterion/criterion.h>
   #include "q_shared.h"

   Test(my_suite, does_the_thing) {
       cr_assert_eq(CG_GetMilliseconds("2:21"), 141000);
   }
   ```

2. Add `<name>` to `SUITES` in the `Makefile`, and add an `SRC_<name>` line
   listing the engine translation units the suite needs.

Keep that source list short. A long one means the unit under test is poorly
isolated, and the link errors tell you exactly which dependency it grew — that
feedback is useful, so don't paper over it by linking everything.

## Testing code that calls the engine

C has no dependency injection, so the standard approach applies: compile the
translation unit under test as-is and satisfy its externals at link time.

- `support/engine_stubs.c` — `Com_Error`, `Com_Printf`, `cl_shownet`. Inert.
- `support/cgame_stubs.c` — defines `cg`, `cgs`, `cg_music`, `CG_Printf`, and the
  `trap_Cvar_*` / `trap_S_*` syscalls.
- `support/fake_fs.c` — an in-memory `trap_FS_*`. This one is a **fake**, not a
  stub: tests declare their input inline and can assert on the result.

  ```c
  fake_fs_reset();
  fake_fs_add("music/playlist.cfg", "battle{\n\ttrack\t0:10\n}\n");
  CG_ParsePlaylist();
  cr_assert_eq(fake_fs_leak_count(), 0);   /* handle was closed */
  ```

  No fixture files on disk, no dependency on the working directory, and file
  content sits next to the assertions that depend on it.

Anything a test needs to *observe* belongs in a purpose-built fake. Stubs are
only there to satisfy the linker.

## The TDD loop

1. Write a failing test that names the defect.
2. Run `make -C tests <suite>` and confirm it fails **for the reason you expect** —
   a wrong assertion and a genuine bug both show red.
3. Fix the code.
4. Re-run. Everything else must stay green.

Two rules worth internalising, both learned the hard way here:

**Never assert against a copy of the buggy expression.** A test that duplicates
the defect and asserts it misbehaves passes forever, whether or not the real
code is fixed. Assert on observable behaviour — a round-trip, a bound, a parse
result — so the test flips when the fix lands.

**Know what each layer can see.** A unit test cannot see which `sizeof` a call
site passes; a source lint can. A lint cannot see what a parser does to real
game data; a sanitizer run can. The `weaponName` overflow needed all three, and
any one alone would have missed it.

## Lints

`lint/` holds source-level checks for defects that are invisible at runtime
until they aren't:

```bash
python3 tests/lint/check_strncpyz_field_sizes.py
```

It rejects `Q_strncpyz(buf.a, src, sizeof(buf.b))` where `a != b`. That is the
exact shape of the `weaponName` overflow — and it stays a bug even when the two
fields are the same size today, because it becomes an overflow the moment one is
resized. Scoping the rule to *same object, different field* is what keeps it
free of false positives; a looser "destination and sizeof differ" rule flags
mostly legitimate code.

## Related tooling

`Tools/dev/` holds the integration-level tools — `zeq2smoke.sh` (load every map
and assert the game survives), `zeq2sanitize.sh` (full run under ASan/UBSan),
`zeq2shot.sh` (screenshot a settled frame). Unit tests and those are
complementary: unit tests are fast and precise but only see linkable units,
while the smoke and sanitizer runs see the real thing with real data.

Worth knowing: these unit tests found undefined behaviour in `msg.c` at lines
the full-game sanitizer run never reached, because the round-trip tests exercise
31- and 32-bit field widths that the stock maps never transmit.
