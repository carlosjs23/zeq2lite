#!/usr/bin/env bash
# Build the engine + game modules, then stage the modules where the engine
# actually loads them.
#
# Why staging is needed: make emits Base/<mod>$ARCH.dylib (ARCH from `uname -p`,
# e.g. "arm"), but the engine builds its dll name from ARCH_STRING in
# q_platform.h, which is "arm64". So it looks for ZEQ2/cgamearm64.dylib. Without
# this copy you keep testing a stale module and wonder why your fix did nothing.
#
# usage:
#   zeq2build.sh                 # full build + stage
#   zeq2build.sh cgame           # force cgame to recompile, then build + stage
#   zeq2build.sh --stage-only     # skip make, just re-stage existing dylibs
#
# Naming a module (cgame/game/ui) drops its objects first. Do that when the
# object files were produced in a different checkout, because the .d dependency
# files hold absolute paths and make will otherwise think they are up to date.

source "$(dirname "${BASH_SOURCE[0]}")/zeq2env.sh"

MODULES=(cgame game ui)
FORCE=()
STAGE_ONLY=0
MAKE_ARGS=()

for arg in "$@"; do
	case "$arg" in
		cgame|game|ui) FORCE+=("$arg") ;;
		--stage-only) STAGE_ONLY=1 ;;
		*) MAKE_ARGS+=("$arg") ;;
	esac
done

cd "$ZEQ2_ROOT"

declare -A OBJDIR=([cgame]=CGame [game]=Game [ui]=UI)
for m in "${FORCE[@]}"; do
	for d in "$ZEQ2_BUILD/Base/${OBJDIR[$m]}" "$ZEQ2_BUILD/$ZEQ2_GAME/Base/${OBJDIR[$m]}"; do
		if [[ -d "$d" ]]; then
			rm -f "$d"/*.o "$d"/*.d
			echo "forced recompile: cleared $(basename "$(dirname "$d")")/${OBJDIR[$m]} objects"
		fi
	done
done

if (( ! STAGE_ONLY )); then
	echo "=== make ${MAKE_ARGS[*]:-} ==="
	if ! make -j"$(sysctl -n hw.ncpu 2>/dev/null || echo 4)" "${MAKE_ARGS[@]}" 2>&1 | grep -viE "^\s*$" | grep -iE "error|warning: unused|LD |CC |^make.*Error" | tail -25; then
		true # grep finding nothing is not a build failure
	fi
	# Re-run quietly to get make's real status (the pipeline above masks it).
	if ! make -j"$(sysctl -n hw.ncpu 2>/dev/null || echo 4)" "${MAKE_ARGS[@]}" >/dev/null 2>&1; then
		echo "error: build failed - re-run 'make' directly to see the full output" >&2
		exit 1
	fi
fi

echo "=== staging modules into $ZEQ2_GAME/ ==="
staged=0
for m in "${MODULES[@]}"; do
	src="$ZEQ2_BUILD/Base/${m}${ZEQ2_ARCH}.dylib"
	if [[ ! -f "$src" ]]; then
		echo "  skip $m (no $src)"
		continue
	fi
	for suffix in "$ZEQ2_ARCH" arm64; do
		dst="$ZEQ2_BUILD/$ZEQ2_GAME/${m}${suffix}.dylib"
		# The build dir may ship these as symlinks; replace with real files.
		rm -f "$dst"
		cp "$src" "$dst"
	done
	echo "  staged $m -> ${m}${ZEQ2_ARCH}.dylib, ${m}arm64.dylib"
	staged=$((staged + 1))
done

if (( staged == 0 )); then
	echo "error: nothing staged - did the build produce any modules?" >&2
	exit 1
fi
echo "ok: $staged module(s) staged in $ZEQ2_BUILD/$ZEQ2_GAME"
