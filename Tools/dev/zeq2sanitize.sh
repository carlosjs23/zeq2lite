#!/usr/bin/env bash
# Build and run the engine under Address + UndefinedBehaviorSanitizer, then
# group the findings.
#
# ASan works now that the engine links real SDL2 (see the SDL2_PREFIX comment in
# the Makefile's darwin section). It used to be unusable: the build linked
# homebrew's sdl12-compat, whose dlopen chain (engine -> sdl12-compat ->
# sdl2-compat -> SDL3) does not survive ASan's malloc interception, so
# sdl12-compat reported "Failed loading SDL2 library" and aborted before the
# engine produced any output. With real SDL2 there is no shim and no dlopen, and
# a full run reaches "entered the game".
#
# Three gotchas this script encodes:
#
# 1. -fsanitize-recover=address is required. Without it ASan aborts on the first
#    hard memory error, and since there IS one during cgame init you would never
#    reach gameplay - the rest of the run, and all UBSan findings after it, stay
#    invisible. halt_on_error=0 alone does nothing for non-recoverable checks.
#
# 2. detect_odr_violation=0 is required. cgame/game/ui/renderer are separate
#    dylibs that each statically link the same Shared/*.c, so every shared global
#    legitimately exists several times over. Left on, ASan emits ~90 odr-violation
#    reports AND poisons one copy of each duplicated global, which then shows up
#    as bogus global-buffer-overflows in unrelated code (tr_shader.c and friends).
#
# 3. Sys_SigHandler in Engine/sys/sys_main.c installs a handler for SIGABRT, so a
#    sanitizer abort surfaces only as "Sys_SigHandler: caught signal 6" with no
#    diagnostic. If you ever need to debug an abort directly, disable it first.
#
# Also note: ASan misbehaves under a sandboxed shell ("Checking file existence is
# not allowed under sandbox"). Run this outside the sandbox.
#
# Two things to expect when reading ASan output here, neither of which is a bug
# in the engine:
#
# - Intermittent reports of a READ at "0 bytes inside of global variable
#   '.str.NN' defined in Game/UI/*.c" - i.e. reading the first byte of a string
#   literal that is plainly in bounds. These come from the engine dlclose'ing and
#   re-dlopen'ing the VM dylibs (CL_FlushMemory -> CL_StartHunkUsers): ASan
#   poisons an unloaded module's globals and does not reliably unpoison them when
#   the image is remapped at the same address. Whether they appear depends on
#   ASLR. A report whose "is located" line says 0 bytes inside a global big
#   enough for the access is one of these; ignore it.
#
# - Roughly one run in four dies early with SIGSEGV or SIGTERM instead of
#   completing. Just re-run; "reached in-game: yes" tells you the run was good.
#
# usage:
#   zeq2sanitize.sh                 # build (if needed), run 'desert', report
#   zeq2sanitize.sh --map namek
#   zeq2sanitize.sh --rebuild       # force a clean sanitizer rebuild
#   zeq2sanitize.sh --all           # include bundled third-party (libjpeg) noise
#
# Always exits 0: this is a report. ASan findings are real memory errors and
# should be fixed. UBSan findings are a latent-UB inventory, not necessarily live
# bugs - triage by whether the value can reach a bad path.

source "$(dirname "${BASH_SOURCE[0]}")/zeq2env.sh"

MAP=desert
REBUILD=0
SHOW_ALL=0

while [[ $# -gt 0 ]]; do
	case "$1" in
		--map) MAP="$2"; shift 2 ;;
		--rebuild) REBUILD=1; shift ;;
		--all) SHOW_ALL=1; shift ;;
		-h|--help) sed -n '2,26p' "${BASH_SOURCE[0]}"; exit 0 ;;
		*) echo "unknown option: $1" >&2; exit 2 ;;
	esac
done

SAN_BUILD="$ZEQ2_ROOT/Build/Debug-$(uname | tr '[:upper:]' '[:lower:]')-$ZEQ2_ARCH"
SAN_BIN="$SAN_BUILD/ZEQ2.$ZEQ2_ARCH"

cd "$ZEQ2_ROOT"

if (( REBUILD )) || [[ ! -x "$SAN_BIN" ]]; then
	# Object files from a differently-instrumented build cannot be mixed: a
	# leftover ASan .o will fail to link against a UBSan-only runtime.
	echo "=== cleaning sanitizer build dir ==="
	rm -rf "$SAN_BUILD"/{Base,client,ded,renderer,renderersmp,tools} \
	       "$SAN_BUILD"/*.dylib "$SAN_BUILD"/ZEQ2.* "$SAN_BUILD"/ZEQ2Dedicated.* 2>/dev/null || true
	echo "=== building with -fsanitize=address,undefined (this takes a few minutes) ==="
	if ! make debug \
			CFLAGS="-fsanitize=address,undefined -fsanitize-recover=address -fno-omit-frame-pointer -g" \
			LDFLAGS="-fsanitize=address,undefined" > "$ZEQ2_ROOT/.sanitize-build.log" 2>&1; then
		echo "error: sanitizer build failed - see $ZEQ2_ROOT/.sanitize-build.log" >&2
		grep -iE "error:" "$ZEQ2_ROOT/.sanitize-build.log" | head -10 >&2
		exit 1
	fi
	echo "build ok"
fi

# Stage assets + modules. The renderer is loaded from the build dir ROOT (not the
# mod dir) under its ARCH_STRING name, which is why the symlink below matters.
if [[ ! -d "$SAN_BUILD/$ZEQ2_GAME/maps" ]]; then
	echo "=== cloning game data into sanitizer build dir ==="
	cp -Rc "$ZEQ2_BUILD/$ZEQ2_GAME" "$SAN_BUILD/$ZEQ2_GAME" 2>/dev/null \
		|| cp -R "$ZEQ2_BUILD/$ZEQ2_GAME" "$SAN_BUILD/$ZEQ2_GAME"
fi
for m in cgame game ui; do
	if [[ -f "$SAN_BUILD/Base/${m}${ZEQ2_ARCH}.dylib" ]]; then
		cp "$SAN_BUILD/Base/${m}${ZEQ2_ARCH}.dylib" "$SAN_BUILD/$ZEQ2_GAME/${m}${ZEQ2_ARCH}.dylib"
		cp "$SAN_BUILD/Base/${m}${ZEQ2_ARCH}.dylib" "$SAN_BUILD/$ZEQ2_GAME/${m}arm64.dylib"
	fi
done
( cd "$SAN_BUILD" && ln -sf "renderer_opengl1_${ZEQ2_ARCH}.dylib" "renderer_opengl1_arm64.dylib" )

LOG="$SAN_BUILD/sanitize.log"
echo "=== running map '$MAP' under ASan + UBSan ==="
cd "$SAN_BUILD"
mapfile -t base < <(zeq2_base_args)
set +e
ASAN_OPTIONS=detect_leaks=0:detect_odr_violation=0:halt_on_error=0:print_stacktrace=1 \
UBSAN_OPTIONS=print_stacktrace=0:halt_on_error=0 \
	"$SAN_BIN" "${base[@]}" +map "$MAP" +wait 400 +quit >"$LOG" 2>&1
code=$?
set -e
echo "engine $(zeq2_describe_exit "$code")"
if grep -q "entered the game" "$LOG" 2>/dev/null; then
	echo "reached in-game: yes (findings cover load + join + rendered frames)"
else
	echo "reached in-game: NO - coverage stops early, findings are incomplete"
	tail -4 "$LOG"
fi

echo
echo "=== ASan findings (real memory errors - fix these) ==="
asan_total=$(grep -c "ERROR: AddressSanitizer" "$LOG" 2>/dev/null || true)
if [[ "$asan_total" == "0" ]]; then
	echo "  none"
else
	# Report each error with the first frame inside our own code, skipping the
	# interceptor/Q_strncpyz style wrappers that would otherwise group unrelated
	# bugs under one line.
	awk '
		/ERROR: AddressSanitizer/ { kind = $0; sub(/.*ERROR: AddressSanitizer: /, "", kind);
		                            sub(/ on address.*/, "", kind); site = ""; next }
		/^ *#[0-9]+ / && site == "" && $0 !~ /libclang_rt|q_shared\.c/ {
		                            site = $NF; next }
		/^SUMMARY: AddressSanitizer/ { if (kind != "") print "  " kind "  at " site; kind = "" }
	' "$LOG" | sort | uniq -c | sort -rn | sed 's/^ *\([0-9]*\) /  [x\1] /'
fi

echo
echo "=== UBSan findings by site ==="
filter=(cat)
(( SHOW_ALL )) || filter=(grep -viE "jidctint|jdhuff|jccolor|jdcolor|jdsample|jquant|jcphuff|puff\.c|unzip\.c")
total=$(grep -c "runtime error:" "$LOG" 2>/dev/null || true)
grep -oE "[^ /]+\.(c|h):[0-9]+:[0-9]+: runtime error: .*" "$LOG" 2>/dev/null \
	| "${filter[@]}" | sort -u | sed 's/^/  /'

echo
echo "=== counts by file ==="
grep -oE "[^ /]+\.(c|h):[0-9]+:[0-9]+: runtime error:" "$LOG" 2>/dev/null \
	| cut -d: -f1 | sort | uniq -c | sort -rn | sed 's/^/  /'

echo
echo "totals: $asan_total ASan report(s), $total UBSan runtime-error report(s)  (log: $LOG)"
(( SHOW_ALL )) || echo "bundled libjpeg/zlib noise hidden; pass --all to include it"
