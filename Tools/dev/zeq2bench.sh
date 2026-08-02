#!/usr/bin/env bash
# Measure renderer frame time on a scene that is identical every run.
#
# Playback of a recorded demo under `timedemo 1` is the only scene in this game
# that repeats exactly: the engine replays the recorded server snapshots and
# advances cl.serverTime by a fixed 50ms per *rendered* frame, so frame N draws
# the same thing no matter how fast the machine is. Live play does not - a
# duel's AI diverges within a second, and even an idle player drifts.
#
# usage:
#   zeq2bench.sh                              # 3 runs of the checked-in demo
#   zeq2bench.sh --ab r_bloom 1 0             # A/B one cvar, arms interleaved
#   zeq2bench.sh --runs 5 --warmup 90
#   zeq2bench.sh --demo mydemo
#   zeq2bench.sh --record                     # record a fresh demo (see below)
#   zeq2bench.sh -- +set r_bloom 0            # pin extra cvars for every run
#
# Prefer `--ab` to two separate invocations. This machine drifts by 10-15% over
# a few minutes of sustained fullscreen load, which is the same order as the
# effect most renderer changes have; interleaving the arms makes the drift hit
# both equally so the *difference* stays meaningful even when the absolute
# numbers move.
#
# Recording needs the demo to match the engine's protocol, which is branch
# specific (PROTOCOL_VERSION in Shared/qcommon.h). A demo recorded on a branch
# that moved the protocol is not found by a build that did not, and the engine
# reports that as `Not found:` followed by a drop to the main menu. So
# `--record` is how you get a demo for a new branch, not a fallback.

source "$(dirname "${BASH_SOURCE[0]}")/zeq2env.sh"

DEMO=bench
RUNS=3
WARMUP=60
RECORD=0
MAP=desert
VIEWPOS="-32400 -23215 -4463 90"
REC_FRAMES=7500
AB_CVAR=""
AB_A=""
AB_B=""
EXTRA=()
CMDLINE=()

while [[ $# -gt 0 ]]; do
	case "$1" in
		--demo) DEMO="$2"; shift 2 ;;
		--runs) RUNS="$2"; shift 2 ;;
		--warmup) WARMUP="$2"; shift 2 ;;
		--ab) AB_CVAR="$2"; AB_A="$3"; AB_B="$4"; shift 4 ;;
		--record) RECORD=1; shift ;;
		--map) MAP="$2"; shift 2 ;;
		--viewpos) VIEWPOS="$2"; shift 2 ;;
		--frames) REC_FRAMES="$2"; shift 2 ;;
		--) shift; EXTRA=("$@"); break ;;
		-h|--help) sed -n '2,29p' "${BASH_SOURCE[0]}"; exit 0 ;;
		*) echo "unknown option: $1" >&2; exit 2 ;;
	esac
done

zeq2_require_bin
cd "$ZEQ2_BUILD"

# Fullscreen, at the desktop resolution. A windowed run presents through the
# macOS compositor, which puts a floor of roughly 4.9ms on the swap and swamps
# anything you are trying to measure - so this script overrides the windowed
# defaults in zeq2env.sh rather than inheriting them.
: "${ZEQ2_FULLSCREEN:=1}"
: "${ZEQ2_MODE:=-2}"
export ZEQ2_FULLSCREEN ZEQ2_MODE
if [[ "$ZEQ2_FULLSCREEN" != "1" ]]; then
	echo "warning: ZEQ2_FULLSCREEN=$ZEQ2_FULLSCREEN - a windowed run measures the compositor," >&2
	echo "         not the renderer. These numbers are not comparable to fullscreen ones." >&2
fi

# Put the player's saved config back, and put it back *between* runs too.
#
# Almost every cvar worth benchmarking is CVAR_ARCHIVE, and the engine writes
# archived cvars back to zeq2config.cfg at shutdown - including ones that only
# arrived as `+set` on the command line. So one A/B arm silently becomes the
# default for every later run, and the B measurement gets repeated under the
# name of A. Restoring the file is the only fix that holds; resetting named
# cvars afterwards needs updating whenever a caller adds one.
#
# Restoring between runs is not just tidiness: leaving run 1's config in place
# made run 2 abort playback with `Com_Error(code=3): Disconnected from server`.
CONFIG="$ZEQ2_BUILD/$ZEQ2_GAME/zeq2config.cfg"
PINS_DIR="$ZEQ2_BUILD/$ZEQ2_GAME"
CONFIG_BACKUP=""
cleanup() {
	[[ -n "$CONFIG_BACKUP" ]] && cp "$CONFIG_BACKUP" "$CONFIG" && rm -f "$CONFIG_BACKUP"
	rm -f "$PINS_DIR"/zeq2bench-arm*.cfg "$PINS_DIR/zeq2bench-durations.txt"
	return 0
}
trap cleanup EXIT INT TERM
if [[ -f "$CONFIG" ]]; then
	CONFIG_BACKUP="$(mktemp)"
	cp "$CONFIG" "$CONFIG_BACKUP"
fi

# Pin everything that costs frame time at the value the game ships with, on
# every run, so a config that some earlier session already polluted cannot
# quietly move the baseline. Restoring the config protects the *player*; this
# list protects the *measurement*.
#
# These go in a cfg rather than on the command line because
# `Com_ParseCommandLine` keeps only MAX_CONSOLE_LINES (32) `+` lines and drops
# the rest *silently* - with 35 pins the trailing `+demo` disappeared and the
# engine started, loaded nothing and sat at the main menu, which reads exactly
# like a broken demo.
#
# write_pins <file> [extra 'set' lines...]
write_pins() {
	local out="$1"; shift
	cat >"$out" <<-'EOF'
	set r_bloom 1
	set r_bloom_alpha 0.5
	set r_bloom_diamond_size 8
	set r_bloom_intensity 0.5
	set r_bloom_darken 32
	set r_bloom_sample_size 256
	set r_picmip 1
	set r_subdivisions 4
	set r_lodbias 0
	set r_lodscale 75
	set r_lodCurveError 250
	set r_detailtextures 1
	set r_simpleMipMaps 1
	set r_dynamiclight 1
	set r_dlightBacks 1
	set r_flares 0
	set r_drawSun 0
	set r_fastsky 0
	set r_nocurves 0
	set r_vertexLight 0
	set r_ext_compressed_textures 0
	set r_ext_multisample 0
	set r_ext_max_anisotropy 2
	set r_textureMode GL_LINEAR_MIPMAP_NEAREST
	set r_swapInterval 0
	set r_finish 0
	set r_smp 0
	set r_motionBlur 0
	set r_greyscale 0
	set r_gamma 1
	set r_overBrightBits 1
	set cg_shadows 1
	set cg_draw2D 1
	set cg_drawFPS 0
	set com_maxfps 85
	EOF
	# Everything below the pins overrides them, which is what makes the A/B
	# knob and `-- +set ...` work.
	local line
	for line in "$@"; do
		echo "$line" >>"$out"
	done
}

# `-- +set name value` becomes a pin; anything else after `--` stays a command
# line argument.
user_pins=()
i=0
while (( i < ${#EXTRA[@]} )); do
	if [[ "${EXTRA[i]}" == "+set" ]] && (( i + 2 < ${#EXTRA[@]} + 1 )); then
		user_pins+=("set ${EXTRA[i+1]} ${EXTRA[i+2]}")
		i=$(( i + 3 ))
	else
		CMDLINE+=("${EXTRA[i]}")
		i=$(( i + 1 ))
	fi
done

mapfile -t base < <(zeq2_base_args)

if (( RECORD )); then
	LOG="$ZEQ2_BUILD/zeq2bench-record.log"
	write_pins "$PINS_DIR/zeq2bench-arm.cfg" "${user_pins[@]}"
	rm -f "$ZEQ2_GAME/demos/$DEMO.dm_"*
	# devmap, not map: setviewpos refuses to run without cheats. The viewpoint is
	# pinned here rather than left to the map's spawn logic, so re-recording the
	# demo on another branch produces the same scene.
	set +e
	"$ZEQ2_BIN" "${base[@]}" "${CMDLINE[@]}" \
		+set g_synchronousClients 1 +exec zeq2bench-arm.cfg \
		+devmap "$MAP" +wait 500 +setviewpos $VIEWPOS +wait 120 \
		+record "$DEMO" +wait "$REC_FRAMES" +stoprecord +wait 40 +quit >"$LOG" 2>&1
	code=$?
	set -e
	echo "$(zeq2_describe_exit "$code")   log: $LOG"
	zeq2_report_crash_markers "$LOG"
	shopt -s nullglob
	written=("$ZEQ2_GAME/demos/$DEMO.dm_"*)
	shopt -u nullglob
	if (( ${#written[@]} == 0 )); then
		echo "error: no demo was written - see $LOG" >&2
		exit 1
	fi
	echo "recorded $(basename "${written[0]}") ($(wc -c <"${written[0]}" | tr -d ' ') bytes)"
	echo "check it in as GameData/demos/$(basename "${written[0]}") so zeq2build.sh stages it"
	exit 0
fi

DURLOG_REL="zeq2bench-durations.txt"
DURLOG="$PINS_DIR/$DURLOG_REL"
LOG="$ZEQ2_BUILD/zeq2bench.log"
: >"$LOG"

# bench_run <arm-cfg-basename> <label>  ->  prints the trimmed mean to stdout
bench_run() {
	local cfg="$1" label="$2" code attempt

	# One engine launch per attempt. A launch occasionally loses the demo before
	# the first frame: `Com_Error(code=3): Disconnected from server`, which is
	# the `disconnect` that UI_KeyConnect (Game/UI/ui_connect.c) issues when
	# Escape reaches the connect screen. Nothing here presses Escape - the
	# events arrive during the several seconds CL_InitCGame spends not pumping
	# the queue, from the fullscreen handoff as one run replaces the last. So
	# settle first, and treat a lost run as a retry rather than a failure; the
	# scene is deterministic, so a replayed run measures the same frames.
	for attempt in 1 2 3; do
		rm -f "$DURLOG"
		[[ -n "$CONFIG_BACKUP" ]] && cp "$CONFIG_BACKUP" "$CONFIG"
		sleep 2

		# `timedemo` is the cvar; cl_timedemo is only the C variable that holds
		# it, so `+set cl_timedemo 1` sets an unrelated cvar and the run reports
		# nothing. `nextdemo quit` exits when playback ends - otherwise
		# CL_DemoCompleted disconnects to the main menu and the engine sits
		# there, which is what "the timedemo does not play" has always meant.
		set +e
		"$ZEQ2_BIN" "${base[@]}" "${CMDLINE[@]}" \
			+set timedemo 1 +set nextdemo quit +set cl_timedemoLog "$DURLOG_REL" \
			+exec "$cfg" +demo "$DEMO" >>"$LOG" 2>&1
		code=$?
		set -e

		[[ -f "$DURLOG" ]] && break
		echo "  $label: attempt $attempt lost the demo, retrying" >&2
	done

	if [[ ! -f "$DURLOG" ]]; then
		echo "$label: no frame durations written - $(zeq2_describe_exit "$code")" >&2
		if grep -q "^Not found: demos/$DEMO" "$LOG"; then
			echo "       no demo for this build's protocol (Shared/qcommon.h)." >&2
			echo "       record one: Tools/dev/zeq2bench.sh --record --demo $DEMO" >&2
		fi
		zeq2_report_crash_markers "$LOG"
		tail -5 "$LOG" >&2
		exit 1
	fi

	python3 - "$DURLOG" "$WARMUP" "$label" <<'PY'
import sys

path, warmup, label = sys.argv[1], int(sys.argv[2]), sys.argv[3]
samples = []
with open(path) as f:
    for line in f:
        line = line.strip()
        if line and not line.startswith('#'):
            samples.append(int(line))

# The first frames pay for shader compilation, texture upload and the first
# touch of every lightmap. That is real work, but it is not what a renderer
# change moves, and one 300ms frame drags the mean of 1000 samples by a third
# of a millisecond.
kept = sorted(samples[warmup:])
if not kept:
    sys.exit(f"{label}: only {len(samples)} frames, a warmup of {warmup} discarded them all")

n = len(kept)
median = kept[n // 2] if n % 2 else (kept[n // 2 - 1] + kept[n // 2]) / 2
# Frame durations are whole milliseconds (unsigned char, clamped at 255), so one
# sample carries about 8% error on a 12ms frame. The trimmed mean is the number
# to compare: averaging a thousand quantised samples recovers the resolution the
# individual samples do not have, and dropping the tails keeps a single
# scheduler hiccup from moving it.
lo, hi = int(n * 0.05), int(n * 0.95)
trimmed = sum(kept[lo:hi]) / max(1, hi - lo)
p95 = kept[min(n - 1, int(n * 0.95))]

print(f"  {label}: {n} frames  median {median:.1f} ms  trimmed mean {trimmed:.2f} ms"
      f"  p95 {p95:.0f} ms  ({1000.0 / trimmed:.1f} fps)", file=sys.stderr)
print(f"{trimmed:.4f}")
PY
}

# summarise <label> <value...>
summarise() {
	python3 - "$@" <<'PY'
import sys, statistics

label, vals = sys.argv[1], sorted(float(v) for v in sys.argv[2:])
med = statistics.median(vals)
spread = (max(vals) - min(vals)) / med * 100 if med else 0
print(f"{label}: {med:.2f} ms ({1000.0 / med:.1f} fps) "
      f"over {len(vals)} runs, spread {spread:.1f}%")
PY
}

if [[ -n "$AB_CVAR" ]]; then
	write_pins "$PINS_DIR/zeq2bench-arma.cfg" "${user_pins[@]}" "set $AB_CVAR $AB_A"
	write_pins "$PINS_DIR/zeq2bench-armb.cfg" "${user_pins[@]}" "set $AB_CVAR $AB_B"

	a_vals=(); b_vals=()
	for (( run = 1; run <= RUNS; run++ )); do
		a_vals+=("$(bench_run zeq2bench-arma.cfg "run $run  $AB_CVAR $AB_A")")
		b_vals+=("$(bench_run zeq2bench-armb.cfg "run $run  $AB_CVAR $AB_B")")
	done

	echo
	summarise "$AB_CVAR $AB_A" "${a_vals[@]}"
	summarise "$AB_CVAR $AB_B" "${b_vals[@]}"
	python3 - "${#a_vals[@]}" "${a_vals[@]}" "${b_vals[@]}" <<'PY'
import sys, statistics

# Compare the arms *pair by pair*, not median against median. The absolute
# numbers wander by tens of percent across a session as the machine heats up or
# something else takes the GPU, and that wander is shared by two runs a minute
# apart - so the ratio within a pair is stable to a few percent even when the
# arms themselves are not. Report the pairwise figure and both spreads, and let
# the reader see when the machine was too busy to trust.
n = int(sys.argv[1])
a = [float(v) for v in sys.argv[2:2 + n]]
b = [float(v) for v in sys.argv[2 + n:]]
deltas = [y - x for x, y in zip(a, b)]
ratios = [y / x for x, y in zip(a, b)]
d, r = statistics.median(deltas), statistics.median(ratios)
lo, hi = min(ratios), max(ratios)
print(f"delta: {d:+.2f} ms ({(r - 1) * 100:+.1f}%) per pair, "
      f"ratio {r:.2f}x (range {lo:.2f}-{hi:.2f})")
PY
else
	write_pins "$PINS_DIR/zeq2bench-arm.cfg" "${user_pins[@]}"
	vals=()
	for (( run = 1; run <= RUNS; run++ )); do
		vals+=("$(bench_run zeq2bench-arm.cfg "run $run")")
	done
	echo
	summarise "frame time" "${vals[@]}"
fi

echo "log: $LOG"
