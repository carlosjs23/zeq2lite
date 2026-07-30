#!/usr/bin/env bash
# Load a map, let it settle, grab an in-engine screenshot, convert it to PNG.
#
# This is the verification loop for anything visual: the engine's own
# `screenshot` command is trustworthy (it reads the backbuffer inside the render
# command queue), whereas macOS `screencapture` needs Screen Recording
# permission that a headless shell usually lacks.
#
# usage:
#   zeq2shot.sh                                   # desert, default settle
#   zeq2shot.sh --map landing --out /tmp/a.png
#   zeq2shot.sh --frames 900 --stats
#   zeq2shot.sh --menu                            # no map: shoot the main menu
#   zeq2shot.sh -- +set cg_thirdPerson 0          # extra engine args after --
#
# Prints the exit status, where the PNG landed, and any crash markers. Use
# --stats for a colour histogram, which is how you tell "world rendered" from
# "flat grey frame" without eyeballing anything.

source "$(dirname "${BASH_SOURCE[0]}")/zeq2env.sh"

MAP=desert
FRAMES=700
OUT=""
STATS=0
MENU=0
EXTRA=()

while [[ $# -gt 0 ]]; do
	case "$1" in
		--map) MAP="$2"; shift 2 ;;
		--frames) FRAMES="$2"; shift 2 ;;
		--out) OUT="$2"; shift 2 ;;
		--stats) STATS=1; shift ;;
		--menu) MENU=1; shift ;;
		--) shift; EXTRA=("$@"); break ;;
		-h|--help) sed -n '2,20p' "${BASH_SOURCE[0]}"; exit 0 ;;
		*) echo "unknown option: $1" >&2; exit 2 ;;
	esac
done

zeq2_require_bin
cd "$ZEQ2_BUILD"

SHOTDIR="$ZEQ2_GAME/screenshots"
LOG="$ZEQ2_BUILD/zeq2shot.log"
rm -rf "$SHOTDIR"

# Build the console command chain. `wait N` defers the rest of the startup
# buffer by N frames, which is what lets us shoot a settled in-game frame.
cmds=()
if (( ! MENU )); then
	cmds+=(+map "$MAP")
fi
cmds+=(+wait "$FRAMES" +screenshot +wait 40 +quit)

mapfile -t base < <(zeq2_base_args)
set +e
"$ZEQ2_BIN" "${base[@]}" "${EXTRA[@]}" "${cmds[@]}" >"$LOG" 2>&1
code=$?
set -e

echo "$(zeq2_describe_exit "$code")   log: $LOG"
zeq2_report_crash_markers "$LOG"

shopt -s nullglob
shots=("$SHOTDIR"/*.tga)
shopt -u nullglob
if (( ${#shots[@]} == 0 )); then
	echo "error: no screenshot was written - the run probably died before '+wait $FRAMES' elapsed" >&2
	tail -5 "$LOG" >&2
	exit 1
fi

newest="${shots[-1]}"
[[ -n "$OUT" ]] || OUT="${newest%.tga}.png"
args=("$newest" "$OUT")
(( STATS )) && args+=(--stats)
python3 "$ZEQ2_DEV/tga2png.py" "${args[@]}"
