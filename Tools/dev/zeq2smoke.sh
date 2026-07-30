#!/usr/bin/env bash
# Regression gate: load every map and assert the game survives joining it.
#
# Drives the CLIENT, not the dedicated server. That is deliberate: the
# dedicated build excludes cl_cgame.o, so it never loads the cgame module and
# cannot catch client-side crashes - which is where the join-time crashes in
# this codebase have actually lived. Use --dedicated to *additionally* exercise
# the server binary for server-only regressions.
#
# usage:
#   zeq2smoke.sh                          # every map, 25s each
#   zeq2smoke.sh --seconds 60
#   zeq2smoke.sh --maps "desert namek"
#   zeq2smoke.sh --dedicated              # also smoke-test the server binary
#   zeq2smoke.sh -- +set s_musicvolume 0  # extra engine args after --
#
# Exit status: 0 if every map passed, 1 otherwise. Suitable for CI.

source "$(dirname "${BASH_SOURCE[0]}")/zeq2env.sh"

SECONDS_PER_MAP=25
MAPS=""
DO_DEDICATED=0
EXTRA=()

while [[ $# -gt 0 ]]; do
	case "$1" in
		--seconds) SECONDS_PER_MAP="$2"; shift 2 ;;
		--maps) MAPS="$2"; shift 2 ;;
		--dedicated) DO_DEDICATED=1; shift ;;
		--) shift; EXTRA=("$@"); break ;;
		-h|--help) sed -n '2,19p' "${BASH_SOURCE[0]}"; exit 0 ;;
		*) echo "unknown option: $1" >&2; exit 2 ;;
	esac
done

zeq2_require_bin
cd "$ZEQ2_BUILD"

if [[ -z "$MAPS" ]]; then
	shopt -s nullglob
	for bsp in "$ZEQ2_GAME"/maps/*.bsp; do
		MAPS+="$(basename "${bsp%.bsp}") "
	done
	shopt -u nullglob
fi
if [[ -z "${MAPS// }" ]]; then
	echo "error: no maps found in $ZEQ2_BUILD/$ZEQ2_GAME/maps" >&2
	exit 1
fi

LOGDIR="$ZEQ2_BUILD/smoke"
mkdir -p "$LOGDIR"
mapfile -t base < <(zeq2_base_args)

declare -a RESULTS
failures=0

run_one() {
	local bin="$1" label="$2" map="$3" log="$4"
	local pid elapsed step alive code
	"$bin" "${base[@]}" "${EXTRA[@]}" +map "$map" >"$log" 2>&1 &
	pid=$!
	elapsed=0
	alive=1
	while (( elapsed < SECONDS_PER_MAP )); do
		step=$(( SECONDS_PER_MAP - elapsed < 5 ? SECONDS_PER_MAP - elapsed : 5 ))
		sleep "$step"
		elapsed=$(( elapsed + step ))
		if ! kill -0 "$pid" 2>/dev/null; then alive=0; break; fi
	done
	if (( alive )); then
		kill "$pid" 2>/dev/null || true
		wait "$pid" 2>/dev/null || true
		# Reaching the deadline is the pass condition: the process was still
		# rendering/simulating when we stopped it.
		if grep -q "entered the game" "$log" 2>/dev/null || [[ "$label" == ded ]]; then
			RESULTS+=("PASS  $label  $map  (survived ${SECONDS_PER_MAP}s)")
			return 0
		fi
		RESULTS+=("FAIL  $label  $map  (alive but never entered the game)")
		return 1
	fi
	wait "$pid" 2>/dev/null
	code=$?
	RESULTS+=("FAIL  $label  $map  (died after ~${elapsed}s, $(zeq2_describe_exit "$code"))")
	return 1
}

echo "smoke: maps [${MAPS% }] at ${SECONDS_PER_MAP}s each"
for map in $MAPS; do
	printf '  client  %-12s ... ' "$map"
	if run_one "$ZEQ2_BIN" client "$map" "$LOGDIR/client-$map.log"; then
		echo "pass"
	else
		echo "FAIL"
		failures=$((failures + 1))
		zeq2_report_crash_markers "$LOGDIR/client-$map.log"
	fi
done

if (( DO_DEDICATED )); then
	ded="$ZEQ2_BUILD/ZEQ2Dedicated.$ZEQ2_ARCH"
	if [[ -x "$ded" ]]; then
		for map in $MAPS; do
			printf '  ded     %-12s ... ' "$map"
			if run_one "$ded" ded "$map" "$LOGDIR/ded-$map.log"; then
				echo "pass"
			else
				echo "FAIL"
				failures=$((failures + 1))
				zeq2_report_crash_markers "$LOGDIR/ded-$map.log"
			fi
		done
	else
		echo "  (skipping dedicated: $ded not built)"
	fi
fi

echo
printf '%s\n' "${RESULTS[@]}"
echo
if (( failures )); then
	echo "SMOKE FAILED: $failures case(s). Logs in $LOGDIR"
	exit 1
fi
echo "SMOKE PASSED: ${#RESULTS[@]} case(s). Logs in $LOGDIR"
