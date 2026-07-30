#!/usr/bin/env bash
# Sweep the aura's tuning and capture what each setting actually looks like.
#
# The aura is described entirely by numbers in the tier config - amplitude,
# wavelength, scroll speed, scale, padding, colour - and none of them mean
# anything until you see the result. Iterating by hand costs a launch, a spawn,
# holding the powerup key and a screenshot, per value.
#
# This renders in the engine rather than approximating the shader elsewhere. The
# shape comes out of glsl/aura_vp.glsl against the player's real screen-space
# bounding box, so what you get back is the aura, not a drawing of one. The cost
# is a game launch per sample, which is why it batches.
#
# Nothing has to be pressed because auraAlways makes the aura draw on its own
# (cg_players.c:2101); the script sets it for the duration and restores the
# config afterwards.
#
# usage:
#   zeq2aura.sh --sweep auraAmplitude=0.5,1,2,4
#   zeq2aura.sh --sweep auraScrollSpeed=0,1.5,4 --set auraAmplitude=2
#   zeq2aura.sh --set auraColor="1 0.2 0.2" --out /tmp/red.png
#   zeq2aura.sh --sweep auraScale=1,2,3 --map landing --hull
#   zeq2aura.sh --sweep auraScale=1,2,3 --crop none      # keep whole frames
#   zeq2aura.sh --sweep auraScale=1,2,3 --range 160      # pull the camera back
#
# Writes a labelled contact sheet, one cell per sample.

source "$(dirname "${BASH_SOURCE[0]}")/zeq2env.sh"

MAP=desert
OUT="$ZEQ2_BUILD/aura-sweep.png"
SWEEP=""
FIXED=()
FRAMES=400
CROP=470x640
RANGE=110
HULL=0
KEEP=0

while [[ $# -gt 0 ]]; do
	case "$1" in
		--sweep)   SWEEP="$2"; shift 2 ;;
		--set)     FIXED+=("$2"); shift 2 ;;
		--map)     MAP="$2"; shift 2 ;;
		--out)     OUT="$2"; shift 2 ;;
		--frames)  FRAMES="$2"; shift 2 ;;
		--crop)    CROP="$2"; shift 2 ;;
		--range)   RANGE="$2"; shift 2 ;;
		--hull)    HULL=1; shift ;;
		--keep)    KEEP=1; shift ;;
		-h|--help) sed -n '2,26p' "${BASH_SOURCE[0]}"; exit 0 ;;
		*) echo "unknown option: $1" >&2; exit 2 ;;
	esac
done

zeq2_require_bin

TIER="$ZEQ2_BUILD/$ZEQ2_GAME/players/tierDefault.cfg"
[[ -f "$TIER" ]] || { echo "error: $TIER not found - run zeq2build.sh first" >&2; exit 1; }

# The config is staged from GameData by every build, so a crash mid-sweep costs
# nothing permanent - but restore it anyway so an interrupted run does not leave
# auraAlways on.
BACKUP="$(mktemp)"
cp "$TIER" "$BACKUP"
cleanup() { cp "$BACKUP" "$TIER"; rm -f "$BACKUP"; }
trap cleanup EXIT INT TERM

# Replace `key value` in the tier config, appending if the key is absent.
set_key() {
	local key="$1" value="$2" file="$3"
	if grep -qE "^[[:space:]]*$key[[:space:]]" "$file"; then
		local tmp; tmp="$(mktemp)"
		awk -v k="$key" -v v="$value" '
			$1 == k { print k " " v; next } { print }
		' "$file" > "$tmp" && mv "$tmp" "$file"
	else
		printf '%s %s\n' "$key" "$value" >> "$file"
	fi
}

if [[ -z "$SWEEP" ]]; then
	SWEEP_KEY="sample"
	SWEEP_VALUES=("only")
else
	SWEEP_KEY="${SWEEP%%=*}"
	IFS=',' read -r -a SWEEP_VALUES <<< "${SWEEP#*=}"
	[[ ${#SWEEP_VALUES[@]} -gt 0 ]] || { echo "error: --sweep has no values" >&2; exit 2; }
fi

WORK="$(mktemp -d)"
[[ $KEEP -eq 1 ]] || trap 'cleanup; rm -rf "$WORK"' EXIT INT TERM

echo "=== aura sweep: $SWEEP_KEY over ${#SWEEP_VALUES[@]} value(s), map $MAP ==="

shots=()
for value in "${SWEEP_VALUES[@]}"; do
	cp "$BACKUP" "$TIER"
	set_key auraExists  True "$TIER"
	set_key auraAlways  True "$TIER"
	for kv in "${FIXED[@]}"; do
		set_key "${kv%%=*}" "${kv#*=}" "$TIER"
	done
	[[ "$SWEEP_KEY" == "sample" ]] || set_key "$SWEEP_KEY" "$value" "$TIER"

	label="$SWEEP_KEY $value"
	safe="$(printf '%s' "$SWEEP_KEY-$value" | tr -c 'A-Za-z0-9._-' '-')"
	shot="$WORK/$safe.png"
	printf '  %-28s ' "$label"

	# Fixed camera and no HUD, so the only thing differing between cells is the
	# aura. The spawn point still varies, so the backdrop does too - that is
	# scenery, not the subject.
	#
	# The camera cvars are passed as bare commands rather than `+set`: they are
	# CVAR_ARCHIVE and sit in zeq2config.cfg, whose exec would overwrite an early
	# `+set`. A command goes into the buffer and runs after that. cg_thirdPersonSlide
	# defaults to -20, which is what pushes the player off to one side.
	if "$ZEQ2_DEV/zeq2shot.sh" --map "$MAP" --frames "$FRAMES" --out "$shot" \
		-- +set cg_auraScreenSpace $(( HULL ? 0 : 1 )) +set cg_draw2D 0 \
		   +cg_thirdPersonSlide 0 +cg_thirdPersonHeight 0 \
		   +cg_thirdPersonAngle 0 +cg_thirdPersonRange "$RANGE" >/dev/null 2>&1 && [[ -f "$shot" ]]; then
		echo "ok"
		shots+=("$shot")
	else
		echo "FAILED (no screenshot)"
	fi
done

if [[ ${#shots[@]} -eq 0 ]]; then
	echo "error: every sample failed - try one by hand with zeq2shot.sh" >&2
	exit 1
fi

cols=2
[[ ${#shots[@]} -eq 1 ]] && cols=1
crop_arg=()
[[ -n "$CROP" && "$CROP" != "none" ]] && crop_arg=(--crop "$CROP")
python3 "$ZEQ2_DEV/png_sheet.py" "$OUT" "${shots[@]}" --columns "$cols" --label --pad 6 "${crop_arg[@]}"

[[ $KEEP -eq 1 ]] && echo "individual frames kept in $WORK"
echo "renderer: $([[ $HULL -eq 1 ]] && echo 'convex hull (cg_auraScreenSpace 0)' || echo 'screen space (cg_auraScreenSpace 1)')"
