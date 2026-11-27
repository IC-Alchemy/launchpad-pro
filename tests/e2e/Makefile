# E2E Host Test Suite & Fuzzing Makefile (tests/e2e/Makefile)
# Exclusive build output directory: ../../build/e2e/ (zero outputs in tests/e2e/)

BUILD_DIR      := ../../build/e2e
MIDI2SOUND_DIR ?= Z:/Codezzz/experiments/MIDI2Sound

E2E_HAS_M1 ?= 0
E2E_HAS_M2 ?= 0
E2E_HAS_M3 ?= 0
FUZZ_RUNS  ?= 100000

ifeq ($(OS),Windows_NT)
  EXE                  := .exe
  HOST_CC              ?= clang
  HOST_CXX             ?= clang++
  FUZZ_CXX             ?= clang++
  HOST_PLATFORM_CFLAGS := -D_CRT_SECURE_NO_WARNINGS
  COMPAT_INC           :=
  M2S_PLATFORM_DEFS    := -DUNICODE -D_UNICODE -DNOMINMAX -DWIN32_LEAN_AND_MEAN -D_CRT_SECURE_NO_WARNINGS
  HOST_LDFLAGS         :=
  ASAN_RES_DIR         := $(subst \,/,$(shell $(HOST_CXX) -print-resource-dir))
  ASAN_DLL             := $(ASAN_RES_DIR)/lib/windows/clang_rt.asan_dynamic-x86_64.dll
  ASAN_DLL_TARGET      := $(BUILD_DIR)/clang_rt.asan_dynamic-x86_64.dll
else
  EXE                  :=
  HOST_CC              ?= gcc
  HOST_CXX             ?= g++
  FUZZ_CXX             ?= clang++
  HOST_PLATFORM_CFLAGS :=
  COMPAT_INC           := -Icompat -Dsscanf_s=sscanf
  M2S_PLATFORM_DEFS    := -DUNICODE -D_UNICODE -DNOMINMAX -DWIN32_LEAN_AND_MEAN
  HOST_LDFLAGS         := -pthread
  ASAN_DLL             :=
  ASAN_DLL_TARGET      :=
endif

SAN_FLAGS      ?= -fsanitize=address,undefined
LIBFUZZ_FLAGS  ?= -fsanitize=fuzzer,address

GATE_DEFS := -DE2E_HAS_M1=$(E2E_HAS_M1) -DE2E_HAS_M2=$(E2E_HAS_M2) -DE2E_HAS_M3=$(E2E_HAS_M3)

# Per-language flags
FW_CFLAGS   := -std=c99 -g3 -O1 -Wall -Werror -DHOST_TEST $(HOST_PLATFORM_CFLAGS) $(SAN_FLAGS) \
               -I../../include -I../../src -I.
FW_CXXFLAGS := -std=c++17 -g3 -O1 -Wall -Wextra -fno-exceptions -fno-rtti -DHOST_TEST \
               $(HOST_PLATFORM_CFLAGS) $(SAN_FLAGS) \
               -include ../../src/midici/vendor/sal_shim.h -I../../include -I../../src
M2S_CXXFLAGS := -std=c++20 -g3 -O1 -Wall -Wextra $(M2S_PLATFORM_DEFS) $(COMPAT_INC) $(SAN_FLAGS) \
                -I$(MIDI2SOUND_DIR)/src -I$(MIDI2SOUND_DIR)/src/ci
E2E_CXXFLAGS := -std=c++20 -g3 -O1 -Wall -Wextra -Werror -DHOST_TEST -DNOMINMAX -DWIN32_LEAN_AND_MEAN \
                $(HOST_PLATFORM_CFLAGS) $(COMPAT_INC) $(SAN_FLAGS) $(GATE_DEFS) \
                -I../../include -I../../src -I$(MIDI2SOUND_DIR)/src -I.

# Track compile-flag / milestone-gate changes so incremental builds stay accurate
FLAGS_VAL   := $(E2E_HAS_M1)-$(E2E_HAS_M2)-$(E2E_HAS_M3)-$(SAN_FLAGS)-$(HOST_CC)-$(HOST_CXX)-$(MIDI2SOUND_DIR)
FLAGS_STAMP := $(BUILD_DIR)/.flags_stamp
ifneq ($(MAKECMDGOALS),clean)
  PREV_FLAGS := $(file < $(FLAGS_STAMP))
  ifneq ($(FLAGS_VAL),$(PREV_FLAGS))
    $(shell mkdir -p "$(BUILD_DIR)")
    $(file > $(FLAGS_STAMP),$(FLAGS_VAL))
  endif
endif

# Verify MIDI2Sound tree is accessible (skip check for clean)
ifneq ($(MAKECMDGOALS),clean)
  ifeq ($(wildcard $(MIDI2SOUND_DIR)/src/ci/ci_agent.cpp),)
    $(error MIDI2SOUND_DIR not found at '$(MIDI2SOUND_DIR)' (missing src/ci/ci_agent.cpp))
  endif
endif

# Firmware C99 sources (exclude src/platform/platform.c which touches STM32 MMIO,
# and src/usb/{usb_desc,usb_prop}.c; e2e_stubs.c replaces tests/stubs/*)
FW_C_SRCS := ../../src/app.c \
             $(wildcard ../../src/instrument/*.c) \
             $(wildcard ../../src/pads/*.c) \
             $(wildcard ../../src/scales/*.c) \
             ../../src/usb/usb_midi2.c \
             ../../src/usb/usb_endp.c \
             $(wildcard ../../src/ump/*.c) \
             $(wildcard ../../src/midici/*.c)

# Firmware C++17 sources (src/midici/*.cpp; excludes vendor/ci_headers_check.cpp)
FW_CXX_SRCS := $(filter-out %/ci_headers_check.cpp, $(wildcard ../../src/midici/*.cpp))

# MIDI2Sound C++20 peer sources
M2S_SRCS := $(MIDI2SOUND_DIR)/src/ci/ci_agent.cpp \
            $(MIDI2SOUND_DIR)/src/common/ump_text.cpp

# E2E suite sources (test_tier{1,2}_m*.cpp, test_tier3_cross.cpp, test_tier4_scenarios.cpp
# are picked up automatically when added; fuzz_ci.cpp has its own target)
E2E_C_SRCS   := e2e_stubs.c
E2E_CXX_SRCS := $(filter-out fuzz_ci.cpp, $(wildcard *.cpp))
FUZZ_SRC     := $(wildcard fuzz_ci.cpp)

# Object paths strictly under ../../build/e2e/
FW_C_OBJS    := $(patsubst ../../src/%.c,$(BUILD_DIR)/obj/fw_c/%.o,$(FW_C_SRCS))
FW_CXX_OBJS  := $(patsubst ../../src/%.cpp,$(BUILD_DIR)/obj/fw_cxx/%.o,$(FW_CXX_SRCS))
E2E_C_OBJS   := $(patsubst %.c,$(BUILD_DIR)/obj/e2e_c/%.o,$(E2E_C_SRCS))
M2S_OBJS     := $(BUILD_DIR)/obj/m2s/ci_agent.o $(BUILD_DIR)/obj/m2s/ump_text.o
E2E_CXX_OBJS := $(patsubst %.cpp,$(BUILD_DIR)/obj/e2e_cxx/%.o,$(E2E_CXX_SRCS))

ALL_E2E_OBJS := $(FW_C_OBJS) $(FW_CXX_OBJS) $(E2E_C_OBJS) $(M2S_OBJS) $(E2E_CXX_OBJS)
DEPS         := $(ALL_E2E_OBJS:.o=.d)

E2E_BIN      := $(BUILD_DIR)/e2e_tests$(EXE)
FUZZ_BIN     := $(BUILD_DIR)/fuzz_ci$(EXE)
LIBFUZZ_BIN  := $(BUILD_DIR)/fuzz_libfuzzer$(EXE)

.PHONY: all test-e2e-smoke test-tier1 test-tier2 test-tier3 test-tier4 test-e2e \
        fuzz-ci fuzz-libfuzzer clean
.DELETE_ON_ERROR:

all: $(E2E_BIN) $(ASAN_DLL_TARGET)

$(FLAGS_STAMP):
	$(if $(wildcard $(dir $@).),,$(shell mkdir -p "$(dir $@)"))
	$(file > $@,$(FLAGS_VAL))
	@true

ifneq ($(ASAN_DLL_TARGET),)
$(ASAN_DLL_TARGET):
	@mkdir -p "$(dir $@)"
	cp -f "$(ASAN_DLL)" "$@"
endif

$(BUILD_DIR)/obj/fw_c/%.o: ../../src/%.c $(FLAGS_STAMP)
	@mkdir -p "$(dir $@)"
	$(HOST_CC) $(FW_CFLAGS) -MMD -MP -c "$<" -o "$@"

$(BUILD_DIR)/obj/fw_cxx/%.o: ../../src/%.cpp $(FLAGS_STAMP)
	@mkdir -p "$(dir $@)"
	$(HOST_CXX) $(FW_CXXFLAGS) -MMD -MP -c "$<" -o "$@"

$(BUILD_DIR)/obj/e2e_c/%.o: %.c $(FLAGS_STAMP)
	@mkdir -p "$(dir $@)"
	$(HOST_CC) $(FW_CFLAGS) -MMD -MP -c "$<" -o "$@"

$(BUILD_DIR)/obj/m2s/ci_agent.o: $(MIDI2SOUND_DIR)/src/ci/ci_agent.cpp $(FLAGS_STAMP)
	@mkdir -p "$(dir $@)"
	$(HOST_CXX) $(M2S_CXXFLAGS) -MMD -MP -c "$<" -o "$@"

$(BUILD_DIR)/obj/m2s/ump_text.o: $(MIDI2SOUND_DIR)/src/common/ump_text.cpp $(FLAGS_STAMP)
	@mkdir -p "$(dir $@)"
	$(HOST_CXX) $(M2S_CXXFLAGS) -MMD -MP -c "$<" -o "$@"

$(BUILD_DIR)/obj/e2e_cxx/%.o: %.cpp $(FLAGS_STAMP)
	@mkdir -p "$(dir $@)"
	$(HOST_CXX) $(E2E_CXXFLAGS) -MMD -MP -c "$<" -o "$@"

$(E2E_BIN): $(ALL_E2E_OBJS)
	@mkdir -p "$(dir $@)"
	$(HOST_CXX) $(SAN_FLAGS) $(ALL_E2E_OBJS) $(HOST_LDFLAGS) -o "$@"

# ---- Test runner targets ----

test-e2e-smoke: $(E2E_BIN) $(ASAN_DLL_TARGET)
	"$(E2E_BIN)" --smoke

test-tier1: $(E2E_BIN) $(ASAN_DLL_TARGET)
	"$(E2E_BIN)" --tier=1

test-tier2: $(E2E_BIN) $(ASAN_DLL_TARGET)
	"$(E2E_BIN)" --tier=2

test-tier3: $(E2E_BIN) $(ASAN_DLL_TARGET)
	"$(E2E_BIN)" --tier=3

test-tier4: $(E2E_BIN) $(ASAN_DLL_TARGET)
	"$(E2E_BIN)" --tier=4

test-e2e: $(E2E_BIN) $(ASAN_DLL_TARGET)
	"$(E2E_BIN)" --all

# ---- Fuzz targets (active once fuzz_ci.cpp exists) ----

ifeq ($(strip $(FUZZ_SRC)),)

fuzz-ci:
	@echo "[e2e] fuzz_ci.cpp not yet present; skipping fuzz-ci."

fuzz-libfuzzer:
	@echo "[e2e] fuzz_ci.cpp not yet present; skipping fuzz-libfuzzer."

else

FUZZ_PRNG_OBJ := $(BUILD_DIR)/obj/fuzz_prng/fuzz_ci.o
FUZZ_LINK_OBJS := $(FW_C_OBJS) $(FW_CXX_OBJS) $(E2E_C_OBJS) $(M2S_OBJS) \
                  $(BUILD_DIR)/obj/e2e_cxx/e2e_harness.o $(FUZZ_PRNG_OBJ)
DEPS += $(FUZZ_PRNG_OBJ:.o=.d)

$(FUZZ_PRNG_OBJ): fuzz_ci.cpp $(FLAGS_STAMP)
	@mkdir -p "$(dir $@)"
	$(HOST_CXX) $(E2E_CXXFLAGS) -MMD -MP -c "$<" -o "$@"

$(FUZZ_BIN): $(FUZZ_LINK_OBJS)
	@mkdir -p "$(dir $@)"
	$(HOST_CXX) $(SAN_FLAGS) $(FUZZ_LINK_OBJS) $(HOST_LDFLAGS) -o "$@"

fuzz-ci: $(FUZZ_BIN) $(ASAN_DLL_TARGET)
	"$(FUZZ_BIN)"

# Mode 2: libFuzzer + ASan uses dedicated coverage-instrumented objects in obj_fuzz/
LF_FW_CFLAGS    := $(filter-out $(SAN_FLAGS),$(FW_CFLAGS)) $(LIBFUZZ_FLAGS)
LF_FW_CXXFLAGS  := $(filter-out $(SAN_FLAGS),$(FW_CXXFLAGS)) $(LIBFUZZ_FLAGS)
LF_M2S_CXXFLAGS := $(filter-out $(SAN_FLAGS),$(M2S_CXXFLAGS)) $(LIBFUZZ_FLAGS)
LF_E2E_CXXFLAGS := $(filter-out $(SAN_FLAGS),$(E2E_CXXFLAGS)) $(LIBFUZZ_FLAGS) -DFUZZ_LIBFUZZER

LF_FW_C_OBJS   := $(patsubst ../../src/%.c,$(BUILD_DIR)/obj_fuzz/fw_c/%.o,$(FW_C_SRCS))
LF_FW_CXX_OBJS := $(patsubst ../../src/%.cpp,$(BUILD_DIR)/obj_fuzz/fw_cxx/%.o,$(FW_CXX_SRCS))
LF_E2E_C_OBJS  := $(patsubst %.c,$(BUILD_DIR)/obj_fuzz/e2e_c/%.o,$(E2E_C_SRCS))
LF_M2S_OBJS    := $(BUILD_DIR)/obj_fuzz/m2s/ci_agent.o $(BUILD_DIR)/obj_fuzz/m2s/ump_text.o
LF_HARNESS_OBJ := $(BUILD_DIR)/obj_fuzz/e2e_cxx/e2e_harness.o
LF_FUZZ_OBJ    := $(BUILD_DIR)/obj_fuzz/e2e_cxx/fuzz_ci.o
LF_ALL_OBJS    := $(LF_FW_C_OBJS) $(LF_FW_CXX_OBJS) $(LF_E2E_C_OBJS) $(LF_M2S_OBJS) $(LF_HARNESS_OBJ) $(LF_FUZZ_OBJ)
DEPS           += $(LF_ALL_OBJS:.o=.d)

$(BUILD_DIR)/obj_fuzz/fw_c/%.o: ../../src/%.c $(FLAGS_STAMP)
	@mkdir -p "$(dir $@)"
	$(HOST_CC) $(LF_FW_CFLAGS) -MMD -MP -c "$<" -o "$@"

$(BUILD_DIR)/obj_fuzz/fw_cxx/%.o: ../../src/%.cpp $(FLAGS_STAMP)
	@mkdir -p "$(dir $@)"
	$(FUZZ_CXX) $(LF_FW_CXXFLAGS) -MMD -MP -c "$<" -o "$@"

$(BUILD_DIR)/obj_fuzz/e2e_c/%.o: %.c $(FLAGS_STAMP)
	@mkdir -p "$(dir $@)"
	$(HOST_CC) $(LF_FW_CFLAGS) -MMD -MP -c "$<" -o "$@"

$(BUILD_DIR)/obj_fuzz/m2s/ci_agent.o: $(MIDI2SOUND_DIR)/src/ci/ci_agent.cpp $(FLAGS_STAMP)
	@mkdir -p "$(dir $@)"
	$(FUZZ_CXX) $(LF_M2S_CXXFLAGS) -MMD -MP -c "$<" -o "$@"

$(BUILD_DIR)/obj_fuzz/m2s/ump_text.o: $(MIDI2SOUND_DIR)/src/common/ump_text.cpp $(FLAGS_STAMP)
	@mkdir -p "$(dir $@)"
	$(FUZZ_CXX) $(LF_M2S_CXXFLAGS) -MMD -MP -c "$<" -o "$@"

$(BUILD_DIR)/obj_fuzz/e2e_cxx/e2e_harness.o: e2e_harness.cpp $(FLAGS_STAMP)
	@mkdir -p "$(dir $@)"
	$(FUZZ_CXX) $(LF_E2E_CXXFLAGS) -MMD -MP -c "$<" -o "$@"

$(BUILD_DIR)/obj_fuzz/e2e_cxx/fuzz_ci.o: fuzz_ci.cpp $(FLAGS_STAMP)
	@mkdir -p "$(dir $@)"
	$(FUZZ_CXX) $(LF_E2E_CXXFLAGS) -MMD -MP -c "$<" -o "$@"

$(LIBFUZZ_BIN): $(LF_ALL_OBJS)
	@mkdir -p "$(dir $@)"
	$(FUZZ_CXX) $(LIBFUZZ_FLAGS) $(LF_ALL_OBJS) $(HOST_LDFLAGS) -o "$@"

fuzz-libfuzzer: $(LIBFUZZ_BIN) $(ASAN_DLL_TARGET)
	"$(LIBFUZZ_BIN)" -runs=$(FUZZ_RUNS)

endif

clean:
	rm -rf "$(BUILD_DIR)"

-include $(DEPS)
