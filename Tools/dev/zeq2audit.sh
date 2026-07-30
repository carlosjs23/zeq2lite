#!/usr/bin/env bash
# Asset audit: find where the code expects content the data set does not ship.
#
# The source tree (SVN r1916, 2015) is newer than the public data release
# (Jan 2012), so several code paths reference assets that were never shipped.
# Every "invisible character" / "black screen" bug found so far has been an
# instance of that. This enumerates them up front instead of one crash at a time.
#
# Two passes:
#   runtime  - launches with com_buildScript 1 + developer 1, which turns the
#              engine's silent asset fallbacks into logged diagnostics, then
#              groups what it finds.
#   static   - checks the specific optional assets the cgame reads directly,
#              which the runtime pass cannot reach when a hard error aborts it.
#
# usage:
#   zeq2audit.sh                     # both passes, map 'desert'
#   zeq2audit.sh --map namek
#   zeq2audit.sh --static-only       # no engine launch (fast)
#   zeq2audit.sh --report out.txt
#
# Always exits 0: this is a report, not a gate. Use zeq2smoke.sh for gating.

source "$(dirname "${BASH_SOURCE[0]}")/zeq2env.sh"

MAP=desert
STATIC_ONLY=0
REPORT=""

while [[ $# -gt 0 ]]; do
	case "$1" in
		--map) MAP="$2"; shift 2 ;;
		--static-only) STATIC_ONLY=1; shift ;;
		--report) REPORT="$2"; shift 2 ;;
		-h|--help) sed -n '2,22p' "${BASH_SOURCE[0]}"; exit 0 ;;
		*) echo "unknown option: $1" >&2; exit 2 ;;
	esac
done

cd "$ZEQ2_BUILD"
DATA="$ZEQ2_BUILD/$ZEQ2_GAME"
OUT=$(mktemp)
trap 'rm -f "$OUT"' EXIT

# Strip Quake colour codes (^1, ^3, ...) so the report is readable.
strip_colour() { sed -E 's/\^[0-9]//g'; }

{
echo "ZEQ2-Lite asset audit"
echo "data dir: $DATA"
echo

# ---------------------------------------------------------------- static pass
echo "=== static: optional assets the cgame reads directly ==="
echo

# Per-tier camera rig. cg_players.c positions tag_cam/tag_camTar off this model;
# cg_tiers.c falls back to players/cameraDefault.md3.
missing_cam=0; present_cam=0
while IFS= read -r tierdir; do
	if [[ -f "$tierdir/camera.md3" ]]; then
		present_cam=$((present_cam + 1))
	else
		missing_cam=$((missing_cam + 1))
	fi
done < <(find "$DATA/players" -mindepth 2 -maxdepth 2 -type d -name "tier*" 2>/dev/null | sort)
echo "camera.md3 (per tier):        present=$present_cam missing=$missing_cam"
if [[ -f "$DATA/players/cameraDefault.md3" ]]; then
	echo "players/cameraDefault.md3:    present"
else
	echo "players/cameraDefault.md3:    MISSING  (global fallback for the above)"
fi

# Camera animation script. Absent => cent->pe.camera.animation stays NULL.
cam_cfg=$(find "$DATA/players" -name "animCam.cfg" 2>/dev/null | wc -l | tr -d ' ')
echo "animCam.cfg:                  found=$cam_cfg  (0 => scripted camera inert)"

# meshScale: optional in tier.cfg, but 0 collapses the model axes and makes
# 1/meshScale divide by zero, so an unset value is not harmless.
total_tier=0; with_scale=0; empty_tier=0
while IFS= read -r cfg; do
	total_tier=$((total_tier + 1))
	[[ -s "$cfg" ]] || empty_tier=$((empty_tier + 1))
	grep -qi "meshScale" "$cfg" 2>/dev/null && with_scale=$((with_scale + 1))
done < <(find "$DATA/players" -name "tier.cfg" 2>/dev/null | sort)
echo "tier.cfg:                     total=$total_tier  set meshScale=$with_scale  zero-byte=$empty_tier"

echo
echo "--- zero-byte tier.cfg files (spawn tier => meshScale 0) ---"
find "$DATA/players" -name "tier.cfg" -size 0 2>/dev/null | sed "s|$DATA/||" | sort || true

echo
echo "--- characters missing an animation.cfg ---"
while IFS= read -r chardir; do
	[[ -f "$chardir/animation.cfg" ]] || echo "  ${chardir#$DATA/}"
done < <(find "$DATA/players" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | sort)

# ---------------------------------------------------------------- runtime pass
if (( ! STATIC_ONLY )); then
	echo
	echo "=== runtime: com_buildScript 1 + developer 1 on map '$MAP' ==="
	echo
	if ! zeq2_require_bin 2>/dev/null; then
		echo "(engine binary missing - skipping runtime pass)"
	else
		log="$ZEQ2_BUILD/zeq2audit.log"
		mapfile -t base < <(zeq2_base_args)
		set +e
		"$ZEQ2_BIN" "${base[@]}" \
			+set com_buildScript 1 +set developer 1 \
			+map "$MAP" +wait 400 +quit >"$log" 2>&1
		code=$?
		set -e
		echo "engine $(zeq2_describe_exit "$code")   log: $log"
		echo

		echo "--- shaders with no image file ---"
		grep -o "Couldn't find image file for shader .*" "$log" 2>/dev/null \
			| strip_colour | sort -u | sed 's/^/  /' || echo "  (none)"

		echo
		echo "--- format fallbacks (requested .png, used .jpg etc) ---"
		n=$(grep -c "not present, using" "$log" 2>/dev/null || true)
		echo "  ${n:-0} occurrence(s) - cosmetic, the shader scripts name the wrong extension"

		echo
		echo "--- defaulted / not-found assets ---"
		grep -iEo "(could not find|using default|using defaults)[^\"]*" "$log" 2>/dev/null \
			| strip_colour | sort | uniq -c | sort -rn | head -20 | sed 's/^/  /' || echo "  (none)"

		echo
		echo "--- engine errors ---"
		grep -iE "ERROR|ERR_DROP|ERR_FATAL" "$log" 2>/dev/null \
			| strip_colour | sort -u | head -10 | sed 's/^/  /' || echo "  (none)"
	fi
fi

echo
echo "=== summary ==="
echo "Any 'missing'/'zero-byte'/'found=0' line above is a code-expects-content gap."
echo "Fix by either shipping the asset or making the code degrade (see"
echo "cg_players.c meshScale and camera.hModel handling for the pattern)."
} | tee "$OUT"

if [[ -n "$REPORT" ]]; then
	cp "$OUT" "$REPORT"
	echo
	echo "report written to $REPORT"
fi
