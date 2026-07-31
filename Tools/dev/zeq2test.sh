#!/usr/bin/env bash
# Test runner. Fast static gates by default; opt in to the slower dynamic ones.
#
# usage:
#   zeq2test.sh              # static gates only (fast, no build)
#   zeq2test.sh --unit       # also build+run the ASan unit tests
#   zeq2test.sh --asan       # also assert the sanitizer run log is clean
#   zeq2test.sh --all        # everything
#
# Exit status: 0 if every selected check passed, 1 otherwise. CI-usable.
#
# The --asan check reads the log left by Tools/dev/zeq2sanitize.sh rather than
# re-running it, because that build+run takes minutes. Run zeq2sanitize.sh first.

source "$(dirname "${BASH_SOURCE[0]}")/zeq2env.sh"

DO_DEMO=0
DO_ASAN=0

while [[ $# -gt 0 ]]; do
	case "$1" in
		--unit) DO_DEMO=1; shift ;;
		--asan) DO_ASAN=1; shift ;;
		--all) DO_DEMO=1; DO_ASAN=1; shift ;;
		-h|--help) sed -n '2,13p' "${BASH_SOURCE[0]}"; exit 0 ;;
		*) echo "unknown option: $1" >&2; exit 2 ;;
	esac
done

cd "$ZEQ2_ROOT"
# tests/ at the root, not Tools/dev/tests: the checks moved there when the
# Criterion suites landed, and this kept pointing at a directory that no longer
# exists - so the gate reported FAIL for "cannot open file" rather than running.
TESTS="$ZEQ2_ROOT/tests"
failures=0
declare -a SUMMARY

record() { # name, status
	SUMMARY+=("$2  $1")
	[[ "$2" == PASS ]] || failures=$((failures + 1))
}

echo "=============================================================="
echo " static gates"
echo "=============================================================="
echo
echo "--- Q_strncpyz sibling-field bounds ---"
if python3 "$TESTS/lint/check_strncpyz_field_sizes.py"; then
	record "strncpyz-field-bounds" PASS
else
	record "strncpyz-field-bounds" FAIL
fi

echo
echo "--- self-aliasing copies ---"
if python3 "$TESTS/lint/check_self_aliasing_copies.py"; then
	record "self-aliasing-copies" PASS
else
	record "self-aliasing-copies" FAIL
fi

if (( DO_DEMO )); then
	echo
	echo "=============================================================="
	echo " unit suites (Criterion, ASan + UBSan)"
	echo "=============================================================="
	# The hand-rolled demo_*.c/test_*.c reproducers this used to compile were
	# absorbed into tests/suites when Criterion landed. Delegate rather than
	# keep a second, drifting copy of how to build them.
	if make -C "$ZEQ2_ROOT" test; then
		record "unit-suites" PASS
	else
		record "unit-suites" FAIL
	fi
fi

if (( DO_ASAN )); then
	echo
	echo "=============================================================="
	echo " sanitizer run log"
	echo "=============================================================="
	SAN_LOG="$ZEQ2_ROOT/Build/Debug-$(uname | tr '[:upper:]' '[:lower:]')-$ZEQ2_ARCH/sanitize.log"
	echo
	if [[ ! -f "$SAN_LOG" ]]; then
		echo "--- SKIP: no log at $SAN_LOG"
		echo "    run Tools/dev/zeq2sanitize.sh first"
	elif ! grep -q "entered the game" "$SAN_LOG"; then
		echo "--- INCONCLUSIVE: that run never reached gameplay, so a clean"
		echo "    result would not mean anything. Re-run zeq2sanitize.sh."
		record "asan-run-clean" FAIL
	else
		n=$(grep -c "ERROR: AddressSanitizer" "$SAN_LOG" 2>/dev/null || true)
		echo "--- ASan reports in last sanitizer run: $n ---"
		if [[ "$n" == "0" ]]; then
			record "asan-run-clean" PASS
		else
			awk '/ERROR: AddressSanitizer/,/SUMMARY: AddressSanitizer/' "$SAN_LOG" \
				| grep -E "ERROR: AddressSanitizer|^ *#[0-9]+ .*\.c:[0-9]+" \
				| grep -vE "libclang_rt" | head -12 | sed 's/^/    /'
			record "asan-run-clean" FAIL
		fi
	fi
fi

echo
echo "=============================================================="
printf '%s\n' "${SUMMARY[@]}"
echo "=============================================================="
if (( failures )); then
	echo "$failures check(s) failing"
	exit 1
fi
echo "all checks passing"
