#!/usr/bin/env bash
# Run a fight and report what the fighters spent to have it.
#
# This is the loop every combat question goes through: put two AI opponents in
# a map, let them fight for N seconds with g_debugFight reporting the resources
# nobody can see on screen, then summarise who spent what. Balance work is
# guesswork without it - a guard that never drains and a pool that never
# converts look identical to a fight that is working.
#
# usage:
#   zeq2duel.sh                                  # goku vs vegetaCell, desert, 90s
#   zeq2duel.sh --seconds 150 --map namek
#   zeq2duel.sh --fighters goku,piccolo
#   zeq2duel.sh --solo                           # one opponent, and you fight it
#   zeq2duel.sh --sample 500                     # finer resource sampling
#   zeq2duel.sh --distance 600                   # both spawn the same way out
#   zeq2duel.sh -- +set g_powerlevel 5000        # extra engine args after --
#
# Reads the fight out of the log at the end: first and last state per fighter,
# how often each defensive verb was used, and the low-water mark of each guard.
# A verb with a count of 0 is one the fight never had a reason to use.

source "$(dirname "${BASH_SOURCE[0]}")/zeq2env.sh"

MAP=desert
SECONDS_TOTAL=90
FIGHTERS="goku,vegetaCell"
SAMPLE=2000
SOLO=0
# Spawn distances. They differ by default so the two do not arrive together,
# but who reaches whom first decides which fighter owns the exchange, so an
# equal pair is how you tell a role assigned by the melee system apart from one
# assigned by the approach.
DIST_A=500
DIST_B=700
EXTRA=()

while [[ $# -gt 0 ]]; do
	case "$1" in
		--map) MAP="$2"; shift 2 ;;
		--seconds) SECONDS_TOTAL="$2"; shift 2 ;;
		--fighters) FIGHTERS="$2"; shift 2 ;;
		--sample) SAMPLE="$2"; shift 2 ;;
		--solo) SOLO=1; shift ;;
		--distance) DIST_A="$2"; DIST_B="$2"; shift 2 ;;
		--) shift; EXTRA=("$@"); break ;;
		-h|--help) sed -n '2,21p' "${BASH_SOURCE[0]}"; exit 0 ;;
		*) echo "unknown option: $1" >&2; exit 2 ;;
	esac
done

zeq2_require_bin
cd "$ZEQ2_BUILD"

# The engine rewrites every archived cvar at shutdown, so a run that sets any
# of them silently edits the player's saved config. Put it back afterwards.
CONFIG="$ZEQ2_BUILD/$ZEQ2_GAME/zeq2config.cfg"
CONFIG_BACKUP=""
if [[ -f "$CONFIG" ]]; then
	CONFIG_BACKUP="$(mktemp)"
	cp "$CONFIG" "$CONFIG_BACKUP"
fi

# Commands with arguments have to reach the engine through a cfg. main()
# re-quotes any argv containing a space and a quoted argument is no longer a
# +command, so `+ai goku 500` on the command line vanishes without a word.
DUEL_CFG="$ZEQ2_BUILD/$ZEQ2_GAME/zeq2duel.cfg"
IFS=',' read -ra names <<<"$FIGHTERS"

{
	echo "g_debugFight $SAMPLE"
	echo "centerview"
	echo "ai ${names[0]} $DIST_A"
	if (( ! SOLO )); then
		echo "wait 20"
		echo "ai ${names[1]:-${names[0]}} $DIST_B"
		# Spectating takes the human out of the fight, and following puts the
		# camera on one of them - free spectate parks you at a spawn point
		# looking at nothing.
		echo "wait 20"
		echo "team spectator"
		echo "wait 60"
		echo "follownext"
	fi
} >"$DUEL_CFG"

cleanup() {
	[[ -n "$CONFIG_BACKUP" ]] && cp "$CONFIG_BACKUP" "$CONFIG" && rm -f "$CONFIG_BACKUP"
	rm -f "$DUEL_CFG"
}
trap cleanup EXIT INT TERM

LOG="$ZEQ2_BUILD/zeq2duel.log"
mapfile -t base < <(zeq2_base_args)

# devmap, because spawning an opponent is cheat-gated like give and noclip.
"$ZEQ2_BIN" "${base[@]}" "${EXTRA[@]}" +devmap "$MAP" +wait 400 +exec zeq2duel.cfg >"$LOG" 2>&1 &
pid=$!

echo "duel: ${names[0]} vs ${names[1]:-you} on $MAP for ${SECONDS_TOTAL}s   log: $LOG"

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
done

if (( alive )); then
	kill "$pid" 2>/dev/null || true
	wait "$pid" 2>/dev/null || true
	status=0
else
	wait "$pid" 2>/dev/null
	code=$?
	echo "FAILED: the engine died after ~${elapsed}s - $(zeq2_describe_exit "$code")"
	echo "--- last 8 log lines ---"
	tail -8 "$LOG"
	zeq2_report_crash_markers "$LOG"
	exit 1
fi

zeq2_report_crash_markers "$LOG"

if ! grep -q "^fight c" "$LOG"; then
	echo "no fight lines in the log - g_debugFight did not report."
	echo "the module may be stale: build with Tools/dev/zeq2build.sh from the repo root."
	exit 1
fi

echo
for who in $(grep -o "^fight c[0-9]*" "$LOG" | sort -u | sed 's/fight c//' | sort -n); do
	samples=$(grep -c "^fight c$who " "$LOG")
	# One sample is someone who left before the fight started - the spectating
	# human reports once and then stops thinking.
	if (( samples < 2 )); then
		continue
	fi
	first=$(grep -m1 "^fight c$who " "$LOG")
	last=$(grep "^fight c$who " "$LOG" | tail -1)
	echo "--- client $who ---"
	echo "  opened  ${first#fight c$who }"
	echo "  closed  ${last#fight c$who }"
	printf '  guard low-water %s\n' \
		"$(grep "^fight c$who " "$LOG" | grep -o 'fatigue [0-9]*' | cut -d' ' -f2 | sort -n | head -1)"
	# Read the running totals out of the last line rather than counting the
	# samples that happened to catch a verb. A zanzoken is up for a few hundred
	# milliseconds and the samples are seconds apart, so counting samples
	# reports the burst verbs as unused however often they are used.
	printf '  used: block %s   zanzoken %s   quickzan %s   boost %s   struggle %s   died %s\n' \
		"$(sed -E 's/.* block [0-9]+\/([0-9]+) .*/\1/' <<<"$last")" \
		"$(sed -E 's/.* zanzoken [0-9]+\/([0-9]+) .*/\1/' <<<"$last")" \
		"$(sed -E 's/.* quickzan [0-9]+\/([0-9]+) .*/\1/' <<<"$last")" \
		"$(sed -E 's/.* boost [0-9]+\/([0-9]+) .*/\1/' <<<"$last")" \
		"$(sed -E 's/.* struggle [0-9]+\/([0-9]+) .*/\1/' <<<"$last")" \
		"$(grep -c "^fight c$who .*dead yes" "$LOG")"
	# Which end of the exchanges this fighter was on. Both read melee 1 the whole
	# time, so only these two separate the fighter opening exchanges from the one
	# they are opened against.
	printf '  role: initiated %s   struck %s   charged %s   fired %s\n' \
		"$(sed -E 's/.* initiate [0-9]+\/([0-9]+) .*/\1/' <<<"$last")" \
		"$(sed -E 's/.* struck [0-9]+\/([0-9]+) .*/\1/' <<<"$last")" \
		"$(sed -E 's/.* charge [0-9]+\/([0-9]+) .*/\1/' <<<"$last")" \
		"$(sed -E 's/.* fired [0-9]+\/([0-9]+) .*/\1/' <<<"$last")"
done

echo
echo "'used' counts are running totals kept every frame, not samples, so a verb"
echo "that is only up for an instant still shows. A zero is a verb the fight"
echo "never wanted."
