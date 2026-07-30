#!/usr/bin/env bash
# Soak-test a run: join a map, stay alive for N seconds, report how it ended.
#
# Use this for "does it still crash?" checks. Timer-driven code (music track
# changes, fades, respawns) only fires after tens of seconds, so a short run can
# look healthy while a longer one still aborts.
#
# usage:
#   zeq2run.sh                       # desert, 60s
#   zeq2run.sh --seconds 180
#   zeq2run.sh --map landing --seconds 90
#   zeq2run.sh --grep "background|ogg"      # also report matching log lines
#   zeq2run.sh -- +set s_musicvolume 0.1    # extra engine args after --
#
# Exit status: 0 if the engine was still running at the deadline (or exited 0),
# non-zero if it died early - so this is usable as a regression gate.

source "$(dirname "${BASH_SOURCE[0]}")/zeq2env.sh"

MAP=desert
SECONDS_TOTAL=60
GREP_PAT=""
EXTRA=()

while [[ $# -gt 0 ]]; do
	case "$1" in
		--map) MAP="$2"; shift 2 ;;
		--seconds) SECONDS_TOTAL="$2"; shift 2 ;;
		--grep) GREP_PAT="$2"; shift 2 ;;
		--) shift; EXTRA=("$@"); break ;;
		-h|--help) sed -n '2,18p' "${BASH_SOURCE[0]}"; exit 0 ;;
		*) echo "unknown option: $1" >&2; exit 2 ;;
	esac
done

zeq2_require_bin
cd "$ZEQ2_BUILD"

LOG="$ZEQ2_BUILD/zeq2run.log"
mapfile -t base < <(zeq2_base_args)

"$ZEQ2_BIN" "${base[@]}" "${EXTRA[@]}" +map "$MAP" >"$LOG" 2>&1 &
pid=$!

interval=15
elapsed=0
alive=1
while (( elapsed < SECONDS_TOTAL )); do
	step=$(( SECONDS_TOTAL - elapsed < interval ? SECONDS_TOTAL - elapsed : interval ))
	sleep "$step"
	elapsed=$(( elapsed + step ))
	if ! kill -0 "$pid" 2>/dev/null; then
		alive=0
		break
	fi
	echo "  t=${elapsed}s alive"
done

if (( alive )); then
	kill "$pid" 2>/dev/null || true
	wait "$pid" 2>/dev/null || true
	echo "ok: survived ${SECONDS_TOTAL}s in '$MAP'   log: $LOG"
	status=0
else
	wait "$pid" 2>/dev/null
	code=$?
	echo "FAILED: died after ~${elapsed}s - $(zeq2_describe_exit "$code")   log: $LOG"
	echo "--- last 8 log lines ---"
	tail -8 "$LOG"
	status=1
fi

zeq2_report_crash_markers "$LOG"
if [[ -n "$GREP_PAT" ]]; then
	echo "--- log lines matching /$GREP_PAT/ ---"
	grep -inE "$GREP_PAT" "$LOG" | tail -12 || echo "  (no matches)"
fi
exit $status
