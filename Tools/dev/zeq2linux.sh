#!/usr/bin/env bash
#
# Run a build or the test suite against Linux, from a mac.
#
#   zeq2linux.sh build            # full release build, engine + modules + QVMs
#   zeq2linux.sh test             # lint + unit suites under ASan/UBSan
#   zeq2linux.sh lint
#   zeq2linux.sh shell            # poke around
#   zeq2linux.sh -- make foo      # anything else
#
#   --arch arm64|amd64            # default arm64: it runs native on Apple
#                                 # silicon. amd64 is what CI runs, but under
#                                 # emulation it is minutes where arm64 is
#                                 # about one - keep it for a final check.
#
# Why bother: darwin-arm never compiles the QVM bytecode (no arm64 JIT, so the
# Makefile sets BUILD_GAME_QVM=0), and ld64 tolerates undefined symbols GNU ld
# rejects. Two live defects sat in this tree behind exactly those two gaps.
#
# BUILD_GAME_QVM=1 is forced below so the q3lcc/q3asm path is covered on either
# arch. HAVE_VM_COMPILED gates it off everywhere but x86_64, but that cvar is
# about whether the *engine* has a bytecode JIT; the toolchain is an ordinary
# cross compiler and emits the same bytecode wherever it runs.
#
# Build outputs go to Build/linux-<arch>/ so they never collide with the mac
# tree, and each arch keeps its own tests/bin - the suites are native binaries
# and running an x86_64 one under arm64 gets you a rosetta error, not a failure.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
IMAGE_BASE=zeq2-linux
ARCH=arm64
CMD=""

while [ $# -gt 0 ]; do
	case "$1" in
		--arch)   ARCH="$2"; shift 2 ;;
		--amd64)  ARCH=amd64; shift ;;
		--arm64)  ARCH=arm64; shift ;;
		--rebuild) REBUILD=1; shift ;;
		--)       shift; CMD="$*"; break ;;
		build|test|lint|shell|clean) CMD="$1"; shift ;;
		*) echo "unknown argument: $1" >&2; exit 2 ;;
	esac
done

[ -n "$CMD" ] || CMD=build

case "$ARCH" in
	amd64|arm64) ;;
	*) echo "--arch must be amd64 or arm64" >&2; exit 2 ;;
esac

IMAGE="${IMAGE_BASE}:${ARCH}"
BUILD_DIR="/src/Build/linux-${ARCH}"
# The suites are native binaries, so each arch needs its own output directory.
TEST_BUILD="bin-linux-${ARCH}"

docker info >/dev/null 2>&1 || {
	echo "docker is not running - start Docker Desktop first" >&2
	exit 1
}

# Rebuilt only when the Dockerfile changes: docker's own layer cache makes a
# no-op build a couple of seconds, and the apt install is the expensive part.
if [ -n "${REBUILD:-}" ] || ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
	echo "==> building $IMAGE"
	docker build --platform "linux/${ARCH}" \
		-t "$IMAGE" -f "$ROOT/Tools/dev/Dockerfile.linux" "$ROOT/Tools/dev"
fi

run() {
	docker run --rm -it --platform "linux/${ARCH}" \
		-v "$ROOT":/src -w /src "$IMAGE" "$@"
}

# -it breaks in a non-tty (CI, a pipe); fall back without it.
if [ ! -t 0 ]; then
	run() {
		docker run --rm --platform "linux/${ARCH}" \
			-v "$ROOT":/src -w /src "$IMAGE" "$@"
	}
fi

case "$CMD" in
	build)
		echo "==> building for linux/${ARCH} into Build/linux-${ARCH}"
		# nproc inside the container: it reports the CPUs docker actually gave
		# this container, which is what make should size itself against. The
		# host's hw.ncpu is a different number and not the binding one.
		run bash -c "make BUILD_DIR=$BUILD_DIR BUILD_GAME_QVM=1 -j\$(nproc)"
		;;
	lint)
		run make lint
		;;
	test)
		echo "==> lint"
		run make lint
		echo "==> unit suites (linux/${ARCH}, ASan + UBSan)"
		run make -C tests "BUILD=$TEST_BUILD" run
		;;
	clean)
		run make "BUILD_DIR=$BUILD_DIR" clean
		run rm -rf "/src/tests/$TEST_BUILD"
		;;
	shell)
		run bash
		;;
	*)
		run bash -c "$CMD"
		;;
esac
