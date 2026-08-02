#!/usr/bin/env bash
# Record a demo once, then replay it through shader variants as video clips.
#
# Screenshots cannot judge animation, and eyeballing a live window cannot
# compare two shaders on the same motion. A demo can: the engine's `video`
# command captures a demo replay to MJPEG-AVI, and a demo replays the same
# entity motion every time - so N replays with N different shaders differ by
# exactly the shader. That is a real A/B, and it is deterministic enough to
# validate future work against a kept reference clip.
#
# usage:
#   zeq2clip.sh --record NAME [--map desert] [--frames 500] [--settle 400]
#               [--set auraKey=value]... [-- +engine args]
#   zeq2clip.sh --play NAME --out clip.gif [--vp FILE] [--fp FILE]
#               [--set auraKey=value]... [-- +engine args]
#
#   --record  join --map, wait --settle frames, record demos/NAME for
#             --frames frames, quit. The recording session's own shader
#             state does not matter - shaders apply at replay time.
#   --input   a cfg of +button/+move commands (with `wait` pacing) exec'd
#             just after recording starts, so the demo captures scripted
#             motion. It has to be a cfg: the engine's command line strips
#             the +/- prefixes, so key commands cannot ride it directly.
#   --play    replay demos/NAME with `video`, write --out. A .gif out needs
#             ffmpeg on PATH; anything else keeps the engine's MJPEG-AVI.
#   --vp/--fp overlay these files as glsl/aura_vp.glsl / aura_fp.glsl in the
#             install for this run only; the installed files come back
#             afterwards. This is the A/B lever - see Tools/dev/aura_variants.
#   --set     set `key value` in the tier config for this run (CRLF kept),
#             restored afterwards. `auraExists=True auraAlways=True` is what
#             makes the aura draw without holding the powerup key.
#
# The player's zeq2config.cfg and tierDefault.cfg are restored after every
# run, so camera cvars passed as bare commands do not leak into the saved
# config (Tools/dev/README.md, trap 1).
#
# example - the aura animation A/B this script was built from:
#   zeq2clip.sh --record abtest --set auraExists=True --set auraAlways=True \
#       -- +cg_auraScreenSpace 1 +model goku/default
#   for v in a-scroll b-sway c-flipbook d-rim-sway; do
#       zeq2clip.sh --play abtest --out /tmp/$v.gif \
#           --vp Tools/dev/aura_variants/$v.vp.glsl \
#           --fp Tools/dev/aura_variants/$v.fp.glsl \
#           -- +cg_auraScreenSpace 1
#   done

source "$(dirname "${BASH_SOURCE[0]}")/zeq2env.sh"

MODE="" NAME="" OUT="" MAP="desert" FRAMES=500 SETTLE=400 VP="" FP="" INPUT=""
SETS=()
EXTRA=()
while [[ $# -gt 0 ]]; do
	case "$1" in
		--record) MODE=record; NAME="$2"; shift 2 ;;
		--play)   MODE=play;   NAME="$2"; shift 2 ;;
		--out)    OUT="$2"; shift 2 ;;
		--map)    MAP="$2"; shift 2 ;;
		--frames) FRAMES="$2"; shift 2 ;;
		--settle) SETTLE="$2"; shift 2 ;;
		--vp)     VP="$2"; shift 2 ;;
		--fp)     FP="$2"; shift 2 ;;
		--input)  INPUT="$2"; shift 2 ;;
		--set)    SETS+=("$2"); shift 2 ;;
		--)       shift; EXTRA=("$@"); break ;;
		*) echo "unknown option: $1" >&2; exit 2 ;;
	esac
done
[[ -n "$MODE" && -n "$NAME" ]] || { sed -n '2,40p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }

zeq2_require_bin || exit 1

GAME="$ZEQ2_BUILD/$ZEQ2_GAME"
CONFIG="$GAME/zeq2config.cfg"
TIER="$GAME/players/tierDefault.cfg"
GLSL="$GAME/glsl"
WORK="$(mktemp -d)"

cleanup() {
	rm -f "$GAME/zeq2clip_input.cfg"
	[[ -f "$WORK/zeq2config.cfg" ]] && cp "$WORK/zeq2config.cfg" "$CONFIG"
	[[ -f "$WORK/tierDefault.cfg" ]] && cp "$WORK/tierDefault.cfg" "$TIER"
	[[ -f "$WORK/aura_vp.glsl" ]] && cp "$WORK/aura_vp.glsl" "$GLSL/aura_vp.glsl"
	[[ -f "$WORK/aura_fp.glsl" ]] && cp "$WORK/aura_fp.glsl" "$GLSL/aura_fp.glsl"
	rm -rf "$WORK"
}
trap cleanup EXIT INT TERM

[[ -f "$CONFIG" ]] && cp "$CONFIG" "$WORK/zeq2config.cfg"

# Replace `key value` in the tier config, appending if the key is absent.
# Matches the file's own line endings - these configs are CRLF and a bare
# print would silently convert them (Tools/dev/README.md, trap 4).
set_key() {
	local key="$1" value="$2" file="$3" eol=''
	if head -c 4096 "$file" | grep -q $'\r'; then
		eol=$'\r'
	fi
	if grep -qE "^[[:space:]]*$key[[:space:]]" "$file"; then
		local tmp; tmp="$(mktemp)"
		awk -v k="$key" -v v="$value" -v eol="$eol" '
			$1 == k { printf "%s %s%s\n", k, v, eol; next } { print }
		' "$file" > "$tmp" && mv "$tmp" "$file"
	else
		printf '%s %s%s\n' "$key" "$value" "$eol" >> "$file"
	fi
}

if [[ ${#SETS[@]} -gt 0 ]]; then
	cp "$TIER" "$WORK/tierDefault.cfg"
	for kv in "${SETS[@]}"; do
		set_key "${kv%%=*}" "${kv#*=}" "$TIER"
	done
fi

if [[ -n "$VP" ]]; then
	cp "$GLSL/aura_vp.glsl" "$WORK/aura_vp.glsl"
	cp "$VP" "$GLSL/aura_vp.glsl"
fi
if [[ -n "$FP" ]]; then
	cp "$GLSL/aura_fp.glsl" "$WORK/aura_fp.glsl"
	cp "$FP" "$GLSL/aura_fp.glsl"
fi

# Windowed, small, HUD off: the subject is the effect, and a fixed mode keeps
# clips comparable across sessions. Camera cvars ride as bare commands - the
# saved config would override +set (README, trap 1) - and are restored by the
# config snapshot above.
base=( $(zeq2_base_args) +set r_mode 3 +cg_draw2D 0
       +cg_thirdPersonSlide 0 +cg_thirdPersonHeight 0
       +cg_thirdPersonAngle 0 +cg_thirdPersonRange 170 )

if [[ "$MODE" == record ]]; then
	inputcmd=()
	if [[ -n "$INPUT" ]]; then
		cp "$INPUT" "$GAME/zeq2clip_input.cfg"
		inputcmd=(+exec zeq2clip_input)
	fi
	"$ZEQ2_BIN" "${base[@]}" "${EXTRA[@]}" +devmap "$MAP" \
		+wait "$SETTLE" +record "$NAME" "${inputcmd[@]}" \
		+wait "$FRAMES" +stoprecord \
		+wait 30 +quit >/dev/null 2>&1
	demo="$(ls "$GAME/demos/$NAME".dm_* 2>/dev/null | head -1)"
	if [[ -z "$demo" ]]; then
		echo "error: no demo recorded (see $ZEQ2_BUILD/zeq2clip.log)" >&2
		exit 1
	fi
	echo "recorded: $demo"
	exit 0
fi

[[ -n "$OUT" ]] || { echo "error: --play needs --out" >&2; exit 2; }
ls "$GAME/demos/$NAME".dm_* >/dev/null 2>&1 || {
	echo "error: no demo named $NAME - record it first" >&2; exit 1; }

rm -f "$GAME/videos/zeq2clip.avi"
# The generous wait outlasts the demo; playback disconnects at demo end,
# which stops the video, and the remaining waits tick down in the menu.
"$ZEQ2_BIN" "${base[@]}" "${EXTRA[@]}" +demo "$NAME" \
	+wait 40 +video zeq2clip +wait 3000 +quit >/dev/null 2>&1

avi="$GAME/videos/zeq2clip.avi"
[[ -s "$avi" ]] || { echo "error: no video captured" >&2; exit 1; }

case "$OUT" in
	*.gif)
		if ! command -v ffmpeg >/dev/null; then
			fallback="${OUT%.gif}.avi"
			mv "$avi" "$fallback"
			echo "no ffmpeg on PATH; kept MJPEG-AVI: $fallback"
			exit 0
		fi
		ffmpeg -y -loglevel error -i "$avi" \
			-vf "fps=18,scale=480:-1:flags=lanczos,split[s0][s1];[s0]palettegen[p];[s1][p]paletteuse" \
			"$OUT"
		rm -f "$avi"
		;;
	*)
		mv "$avi" "$OUT"
		;;
esac
echo "clip: $OUT"
