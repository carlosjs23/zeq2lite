# Unit tests for ZEQ2-Lite, on Criterion.
#
#   make            build and run every suite
#   make build      build only
#   make run        run only
#   make SUITE=msg  build+run one suite
#   make report     run and emit JUnit XML into build/report/
#   make coverage   run with instrumentation and print a per-file summary
#   make list       list every registered test
#   make clean
#
# Sanitizers are ON by default (SAN=0 to disable). ASan and UBSan cost nothing
# noticeable at this scale and this codebase has live memory bugs, so tests are
# the right place to keep them permanently armed.
#
# Criterion runs each test in its own process, so a segfault or sanitizer abort
# fails exactly one test and is reported as a crash rather than taking down the
# run. That is why it was chosen over an in-process framework here.

ROOT              := $(abspath ..)
CRITERION_PREFIX  ?= /opt/homebrew
CC                ?= clang
SAN               ?= 1
BUILD             := bin

# Adding a suite: create suites/test_<name>.c, add <name> here, and give it an
# SRC_<name> line listing the engine translation units it needs. Keep that list
# minimal - a short list is a sign the unit under test is well isolated.
SUITES := q_shared screen cg_screen vidmodes fs_path cg_music cg_players msg weapgfx png snd

SHARED_CORE := $(ROOT)/Shared/q_shared.c $(ROOT)/Shared/q_math.c
SUPPORT     := support/engine_stubs.c

SRC_q_shared := $(SHARED_CORE) $(SUPPORT)
SRC_screen   := $(SHARED_CORE) $(SUPPORT)
SRC_cg_screen := $(ROOT)/Game/CGame/cg_drawtools.c $(SHARED_CORE) $(SUPPORT) \
                support/cgame_stubs.c support/fake_draw.c
SRC_vidmodes := $(ROOT)/Engine/renderer/tr_vidmodes.c $(SHARED_CORE) $(SUPPORT)
# files.c is the least isolated unit here - it reaches cvars, the command buffer,
# the allocators and the platform layer - so its seam is correspondingly wide.
# The zip reader and zlib are linked for real rather than stubbed, because
# fourteen hand-written unz* stubs would drift out of sync with the header.
SRC_fs_path  := $(ROOT)/Shared/files.c $(ROOT)/Shared/unzip.c $(ROOT)/Shared/ioapi.c \
                $(ROOT)/Engine/zlib/adler32.c $(ROOT)/Engine/zlib/crc32.c \
                $(ROOT)/Engine/zlib/inffast.c $(ROOT)/Engine/zlib/inflate.c \
                $(ROOT)/Engine/zlib/inftrees.c $(ROOT)/Engine/zlib/zutil.c \
                $(SHARED_CORE) $(SUPPORT) support/fs_stubs.c support/fake_mkdir.c
SRC_cg_music := $(ROOT)/Game/CGame/cg_music.c $(SHARED_CORE) $(SUPPORT) \
                support/cgame_stubs.c support/fake_fs.c
SRC_cg_players := $(ROOT)/Game/CGame/cg_players.c $(SHARED_CORE) $(SUPPORT) \
                support/cgame_stubs.c support/cgplayers_stubs.c support/fake_lerptag.c
SRC_msg      := $(ROOT)/Shared/msg.c $(ROOT)/Shared/huffman.c $(SHARED_CORE) $(SUPPORT)
SRC_weapgfx  := $(SHARED_CORE) $(SUPPORT)
SRC_png      := $(ROOT)/Engine/renderer/tr_image_png.c $(ROOT)/Shared/puff.c \
                $(SHARED_CORE) $(SUPPORT)
SRC_snd      := $(ROOT)/Engine/client/snd_dma.c $(SHARED_CORE) $(SUPPORT) \
                support/sound_stubs.c

# The renderer sources reach SDL2's SDL_opengl.h through qgl.h, so the PNG
# suite needs the same SDL2 include path the engine build uses.  See the
# SDL2_PREFIX comment in ../Makefile: `brew install sdl2` gives you sdl2-compat,
# not real SDL2, so this is a pre-migration keg or a from-source build.
#
# An empty match here used to degrade silently into a bare -I/include/SDL2 and
# surface as 'SDL_opengl.h file not found' in the png suite, so it is an error.
SDL2_PREFIX ?= $(lastword $(sort $(wildcard /opt/homebrew/Cellar/sdl2/[0-9]*)))
ifeq ($(strip $(SDL2_PREFIX)),)
  ifneq ($(if $(MAKECMDGOALS),$(filter-out clean,$(MAKECMDGOALS)),build),)
    $(error No real SDL2 found under /opt/homebrew/Cellar/sdl2. \
      Build SDL2 from source and pass its prefix, e.g. make SDL2_PREFIX=$$HOME/sdl2)
  endif
endif
INCLUDES := -I$(ROOT)/Shared -I$(ROOT)/Game/Game -I$(ROOT)/Game/CGame \
            -I$(ROOT)/Engine/renderer -I$(ROOT)/Engine/client -Ifixtures \
            -Isupport -I$(CRITERION_PREFIX)/include \
            -I$(SDL2_PREFIX)/include/SDL2

# -DGAMEDLL keeps the game headers on their native-module path rather than the
# QVM one. -Wno-* entries are blanket-suppressed only where the 2011-era engine
# sources are unavoidably noisy; test code itself must compile clean.
WARN := -Wall -Wno-unused-parameter -Wno-missing-braces
CFLAGS_TEST := -std=gnu99 -g -O0 $(WARN) $(INCLUDES)
LDFLAGS_TEST := -L$(CRITERION_PREFIX)/lib -lcriterion -lm

ifeq ($(SAN),1)
	# -fno-sanitize-recover=undefined is what makes UB a TEST FAILURE. By default
	# UBSan prints and continues, so a suite can report all-green while the code
	# under test is undefined - which is exactly what happened with the PNG
	# misaligned loads. Aborting turns it into a Criterion "CRASH", i.e. red.
	SANFLAGS := -fsanitize=address,undefined -fno-sanitize-recover=undefined \
	            -fno-omit-frame-pointer
	CFLAGS_TEST  += $(SANFLAGS)
	LDFLAGS_TEST += $(SANFLAGS)
endif

ifeq ($(COVERAGE),1)
	CFLAGS_TEST  += -fprofile-instr-generate -fcoverage-mapping
	LDFLAGS_TEST += -fprofile-instr-generate
endif

BINARIES := $(addprefix $(BUILD)/test_,$(SUITES))

.PHONY: all build run report coverage list clean $(SUITES)

all: run

build: $(BINARIES)

# Each suite links only the translation units it needs, so a link error is
# immediate feedback that the unit under test has grown a new dependency.
.SECONDEXPANSION:
$(BUILD)/test_%: suites/test_%.c $$(SRC_%) | $(BUILD)
	@echo "  CC-TEST  $*"
	@$(CC) $(CFLAGS_TEST) -o $@ $< $(SRC_$*) $(LDFLAGS_TEST)

$(BUILD):
	@mkdir -p $(BUILD)

# Run every suite even if an earlier one fails, then fail the target if any did.
# ASAN_OPTIONS: this codebase statically links the same Shared/*.c into several
# modules, so ODR detection reports duplicates that are legitimate here.
RUN_ENV := ASAN_OPTIONS=detect_leaks=0:detect_odr_violation=0 \
           UBSAN_OPTIONS=print_stacktrace=1
CRITERION_ENV := CRITERION_DISABLE_TIME_MEASURE=1

run: build
	@fail=0; \
	for s in $(SUITES); do \
		echo ""; echo "=== $$s ==="; \
		$(RUN_ENV) $(CRITERION_ENV) ./$(BUILD)/test_$$s || fail=1; \
	done; \
	echo ""; \
	if [ $$fail -ne 0 ]; then echo "TESTS FAILED"; exit 1; fi; \
	echo "ALL SUITES PASSED"

report: build
	@mkdir -p $(BUILD)/report
	@fail=0; \
	for s in $(SUITES); do \
		$(RUN_ENV) ./$(BUILD)/test_$$s --xml=$(BUILD)/report/$$s.xml || fail=1; \
	done; \
	echo "JUnit XML in $(BUILD)/report/"; \
	[ $$fail -eq 0 ]

list: build
	@for s in $(SUITES); do ./$(BUILD)/test_$$s --list; done

# Per-suite targets: make msg
$(SUITES): % : $(BUILD)/test_%
	@$(RUN_ENV) $(CRITERION_ENV) ./$(BUILD)/test_$@

coverage:
	@$(MAKE) --no-print-directory COVERAGE=1 SAN=0 build
	@rm -rf $(BUILD)/prof && mkdir -p $(BUILD)/prof
	@for s in $(SUITES); do \
		LLVM_PROFILE_FILE=$(BUILD)/prof/$$s.profraw ./$(BUILD)/test_$$s >/dev/null 2>&1 || true; \
	done
	@xcrun llvm-profdata merge -sparse $(BUILD)/prof/*.profraw -o $(BUILD)/prof/all.profdata
	@echo ""
	@for s in $(SUITES); do \
		xcrun llvm-cov report ./$(BUILD)/test_$$s \
			-instr-profile=$(BUILD)/prof/all.profdata 2>/dev/null \
			| grep -E "Filename|^(Shared|Game|Engine)/|TOTAL" | head -20; \
	done

clean:
	@rm -rf $(BUILD)
