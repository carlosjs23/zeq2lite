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

# Patched GLSL programs, shader scripts and tier config, overlaid onto the
# installed mod directory. Some of it is coupled to the engine -
# glsl/generic_vp.glsl only makes sense against the tcMod handling in
# tr_shade.c - so staging it is part of a build, not a setup step you do once.
# Reinstalling the mod directory drops the stock files back on top; running this
# again restores the overlay. See GameData/README.md.
if [[ -d "$ZEQ2_ROOT/GameData" ]]; then
	echo "=== staging mod files into $ZEQ2_GAME/ ==="
	data=0
	while IFS= read -r -d '' src; do
		rel="${src#"$ZEQ2_ROOT/GameData/"}"
		dst="$ZEQ2_BUILD/$ZEQ2_GAME/$rel"
		mkdir -p "$(dirname "$dst")"
		rm -f "$dst"
		cp "$src" "$dst"
		data=$((data + 1))
	done < <(find "$ZEQ2_ROOT/GameData" -type f ! -name 'README.md' -print0)
	echo "ok: $data mod file(s) staged in $ZEQ2_BUILD/$ZEQ2_GAME"
fi

# The screen-space aura's ring mesh and spike strip are generated rather than
# authored, so the generators are the source and the outputs are build products
# like anything else here. Generating them on every build keeps the two from
# drifting - the strip that shipped under Build/ had been produced by an earlier
# revision of its generator and no longer matched it.
#
# Both scripts are deterministic and take only stdlib, so this is reproducible
# and needs nothing installed. Parameters are the generators' own defaults;
# pass them explicitly only when a value has to differ from the default.
if [[ -d "$ZEQ2_ROOT/GameData" ]] && command -v python3 >/dev/null 2>&1; then
	echo "=== generating aura mesh and texture into $ZEQ2_GAME/ ==="
	mkdir -p "$ZEQ2_BUILD/$ZEQ2_GAME/models/effects" "$ZEQ2_BUILD/$ZEQ2_GAME/effects/aura"
	python3 "$ZEQ2_ROOT/Tools/dev/make_aura_mesh.py" \
		"$ZEQ2_BUILD/$ZEQ2_GAME/models/effects/aura.iqm" \
		--segments 1024 --outline "$ZEQ2_ROOT/Tools/dev/aura_reference.png" >/dev/null
	python3 "$ZEQ2_ROOT/Tools/dev/aura_band_from_reference.py" \
		"$ZEQ2_ROOT/Tools/dev/aura_reference.png" \
		"$ZEQ2_BUILD/$ZEQ2_GAME/effects/aura/auraStrip.png" \
		"$ZEQ2_BUILD/$ZEQ2_GAME/effects/aura/auraStrip.raw" \
		--inner-hug 0.0 --segments 1024 --width 2048 --height 512 --frames 4 >/dev/null
	echo "ok: aura.iqm and auraStrip generated from the reference"

	# The map-selection highlight and the missing-levelshot placeholder, for the
	# same reason: ui_startserver.c names images the mod directory does not
	# supply, and a missing one draws as shader handle 0 - the default black and
	# white block - rather than as nothing.
	python3 "$ZEQ2_ROOT/Tools/dev/make_ui_art.py" \
		"$ZEQ2_BUILD/$ZEQ2_GAME/interface/art" >/dev/null
	echo "ok: maps_selected.png and unknownmap.png generated"
fi
