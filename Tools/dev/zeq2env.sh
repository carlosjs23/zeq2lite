#!/usr/bin/env bash
# Shared locations and helpers for the Tools/dev scripts. Source, don't run.
#
# Override any of these from the environment:
#   ZEQ2_ROOT   repo root (defaults to two levels above this script)
#   ZEQ2_ARCH   engine arch suffix, e.g. arm / x86_64 (default: uname -p)
#   ZEQ2_BUILD  build output dir (default: $ZEQ2_ROOT/Build/Release-darwin-$ZEQ2_ARCH)
#   ZEQ2_GAME   mod/base game dir name under the build dir (default: ZEQ2)
#   ZEQ2_FULLSCREEN  r_fullscreen for a run (default: 0, windowed)
#   ZEQ2_MODE        r_mode for a run (default: 3; -2 is the desktop resolution)
#   ZEQ2_HUNKMEGS    com_hunkMegs for a run (default: 256)

set -euo pipefail

ZEQ2_ROOT="${ZEQ2_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
ZEQ2_ARCH="${ZEQ2_ARCH:-$(uname -p)}"
ZEQ2_BUILD="${ZEQ2_BUILD:-$ZEQ2_ROOT/Build/Release-$(uname | tr '[:upper:]' '[:lower:]')-$ZEQ2_ARCH}"
ZEQ2_GAME="${ZEQ2_GAME:-ZEQ2}"
ZEQ2_BIN="$ZEQ2_BUILD/ZEQ2.$ZEQ2_ARCH"
ZEQ2_DEV="$ZEQ2_ROOT/Tools/dev"

# Engine args that keep a run non-interactive and reproducible: windowed, fixed
# resolution, enough hunk for the stock maps, and the mod dir wired up.
#
# Windowed at r_mode 3 is right for a crash gate and wrong for anything
# measuring the renderer: a windowed run presents through the compositor, so
# these defaults quietly decide what you are timing. Override per run -
# ZEQ2_FULLSCREEN=1 ZEQ2_MODE=-2 zeq2run.sh ... - rather than passing a second
# +set and relying on the later one winning.
zeq2_base_args() {
	printf '%s\n' \
		+set fs_game "$ZEQ2_GAME" \
		+set r_fullscreen "${ZEQ2_FULLSCREEN:-0}" \
		+set r_mode "${ZEQ2_MODE:-3}" \
		+set com_hunkMegs "${ZEQ2_HUNKMEGS:-256}"
}

zeq2_require_bin() {
	if [[ ! -x "$ZEQ2_BIN" ]]; then
		echo "error: engine binary not found at $ZEQ2_BIN" >&2
		echo "       build it first: Tools/dev/zeq2build.sh" >&2
		return 1
	fi
}

# Surface the markers that mean "this run died badly" rather than exited cleanly.
zeq2_report_crash_markers() {
	local log="$1"
	local hits
	hits=$(grep -inE "stack_chk|stack smashing|signal SIG|Abort trap|ERR_FATAL|ERR_DROP|Segmentation|malloc: .*error|assertion" "$log" 2>/dev/null | head -8 || true)
	if [[ -n "$hits" ]]; then
		echo "--- crash markers in $(basename "$log") ---"
		echo "$hits"
	fi
}

# 128+N means killed by signal N; 134 = SIGABRT (the stack-protector abort).
zeq2_describe_exit() {
	local code="$1"
	case "$code" in
		0) echo "exit 0 (clean)" ;;
		134) echo "exit 134 (SIGABRT - likely __stack_chk_fail / abort)" ;;
		139) echo "exit 139 (SIGSEGV)" ;;
		*) if (( code > 128 )); then echo "exit $code (killed by signal $((code - 128)))"; else echo "exit $code"; fi ;;
	esac
}
