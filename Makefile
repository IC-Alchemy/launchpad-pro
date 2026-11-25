BUILDDIR = build

TOOLS = tools

SOURCES += src/app.c

# firmware-only sources (USB MIDI 2.0 transport); not part of the simulator
FW_SOURCES += $(wildcard src/usb/*.c src/ump/*.c src/platform/*.c src/midici/*.c src/pads/*.c src/scales/*.c src/instrument/*.c)

# MIDI-CI: Microsoft's header-only responder (src/midici/vendor) is C++17; the
# check TU compiles it with the firmware flags (no exceptions, no RTTI, no libstdc++)
FW_CXX_SOURCES += src/midici/vendor/ci_headers_check.cpp src/midici/responder.cpp src/midici/pe_session.cpp

INCLUDES += -Iinclude -Isrc

# the vendor archive minus the objects src/usb replaces; c_midi.o gets its
# parser state (DinRx, USBParserToDev) made global for src/usb/usb_midi2.c
ORIG_LIB = lib/launchpad_pro.a
LIB = $(BUILDDIR)/launchpad_pro_trimmed.a
LIB_WORK = $(BUILDDIR)/lib_trim

OBJECTS = $(addprefix $(BUILDDIR)/, $(addsuffix .o, $(basename $(SOURCES) $(FW_SOURCES) $(FW_CXX_SOURCES))))

# host simulator: the app plus stand-ins for the hal and midi2 API
SIM_SOURCES = $(TOOLS)/simulator.c $(TOOLS)/sim_midi2.c $(SOURCES) src/ump/ump_codec.c src/pads/pad_velocity.c \
	src/scales/scales.c src/scales/tuning.c src/scales/tuning_data.c src/instrument/settings.c src/instrument/ui.c src/instrument/io_hal.c src/pads/pad_engine.c src/pads/twostage.c

ifeq ($(OS),Windows_NT)
  EXE = .exe
  HOST_GCC ?= clang
  HOST_CXX ?= clang++
  HOST_PLATFORM_CFLAGS = -D_CRT_SECURE_NO_WARNINGS
  HEXTOSYX_WIN_INC = -I$(BUILDDIR)/win_compat
else
  EXE =
  HOST_GCC ?= gcc
  HOST_CXX ?= g++
  HOST_PLATFORM_CFLAGS =
  HEXTOSYX_WIN_INC =
endif
HOST_GPP ?= $(HOST_CXX) -include cstdint -include string

# output files
SYX = $(BUILDDIR)/launchpad_pro.syx
ELF = $(BUILDDIR)/launchpad_pro.elf
HEX = $(BUILDDIR)/launchpad_pro.hex
HEXTOSYX = $(BUILDDIR)/hextosyx$(EXE)
SIMULATOR = $(BUILDDIR)/simulator$(EXE)
TEST_BIN = $(BUILDDIR)/tests/run_tests$(EXE)
TEST_CI_OBJECT = $(BUILDDIR)/tests/responder.o $(BUILDDIR)/tests/pe_session.o

# host unit tests: later tasks append their files to TEST_SOURCES
TEST_SOURCES += tests/run_tests.c tests/test_harness.c
TEST_SOURCES += tests/test_ump_codec.c src/ump/ump_codec.c
TEST_SOURCES += tests/test_ump_tx.c src/ump/ump_tx.c tests/stubs/stubs.c
TEST_SOURCES += tests/test_ump_stream.c src/ump/ump_stream.c
TEST_SOURCES += tests/test_ump_rx.c src/ump/ump_rx.c
TEST_SOURCES += tests/test_usb_desc.c src/usb/usb_desc.c
TEST_SOURCES += tests/test_usb_prop.c src/usb/usb_prop.c src/usb/usb_midi2.c src/usb/usb_endp.c
TEST_SOURCES += tests/test_usb_data.c
TEST_SOURCES += tests/test_pad_velocity.c src/pads/pad_velocity.c
TEST_SOURCES += tests/test_scales.c tests/test_tuning.c src/scales/scales.c src/scales/tuning.c src/scales/tuning_data.c
TEST_SOURCES += tests/test_settings.c src/instrument/settings.c tests/stubs/io_stub.c
TEST_SOURCES += tests/test_pad_engine.c src/pads/pad_engine.c
TEST_SOURCES += tests/test_pressure.c
TEST_SOURCES += tests/test_ui.c src/instrument/ui.c
TEST_SOURCES += tests/test_pads.c
TEST_SOURCES += tests/test_twostage.c src/pads/twostage.c
TEST_SOURCES += tests/test_midici.c src/midici/midici.c src/midici/ci_queue.c
TEST_SOURCES += tests/test_ci_pe.c src/midici/ci_json.c src/midici/ci_pe.c
GOLDEN_DESC = $(BUILDDIR)/tests/golden_usb_desc_rodata.bin
CI_HEADERS = $(wildcard src/midici/*.h src/midici/vendor/*.h)
TEST_CFLAGS = -std=c99 -Wall -Werror $(HOST_PLATFORM_CFLAGS) -DHOST_TEST -Iinclude -Isrc -Itests -DGOLDEN_DESC=\"$(GOLDEN_DESC)\"

# tools
CC = arm-none-eabi-gcc
CXX = arm-none-eabi-g++
LD = arm-none-eabi-gcc
OBJCOPY = arm-none-eabi-objcopy
AR = arm-none-eabi-ar

CFLAGS  = -Os -Wall -I.\
-D_STM32F103RBT6_  -D_STM3x_  -D_STM32x_ -mthumb -mcpu=cortex-m3 \
-fsigned-char  -DSTM32F10X_MD -DUSE_STDPERIPH_DRIVER -DHSE_VALUE=6000000UL \
-DCMSIS -DUSE_GLOBAL_CONFIG -ffunction-sections -std=c99  -mlittle-endian \
$(INCLUDES)

CXXFLAGS = -Os -Wall -Wextra -mthumb -mcpu=cortex-m3 -mlittle-endian -fsigned-char -std=c++17 -fno-exceptions -fno-rtti -fno-threadsafe-statics -ffunction-sections -fdata-sections -include src/midici/vendor/sal_shim.h $(INCLUDES)

LDSCRIPT = stm32_flash.ld

LDFLAGS += -T$(LDSCRIPT) -u _start -u _Minimum_Stack_Size  -mcpu=cortex-m3 -mthumb -specs=nano.specs -specs=nosys.specs -nostdlib -Wl,-static -N -nostartfiles -Wl,--gc-sections
LDFLAGS += -Wl,--wrap=hal_send_midi,--wrap=hal_send_sysex,--wrap=usbmidi_poll,--wrap=midi_flush_windows_buffer,--wrap=midi_parsers_poll,--wrap=midi_parsers_init

HAS_LIBINTELHEX := $(wildcard $(TOOLS)/libintelhex/include/intelhex.h)

ifeq ($(strip $(HAS_LIBINTELHEX)),)
all: $(HEX) $(SIMULATOR)
	$(SIMULATOR)
	sh $(TOOLS)/check_image.sh $(ELF)
	@echo "note: tools/libintelhex not initialized; skipping $(SYX)"
else
all: $(SYX)
endif

elf: $(ELF)
hex: $(HEX)
simulator: $(SIMULATOR)

# build the final sysex file from the ELF - run the simulator first
$(SYX): $(HEX) $(HEXTOSYX) $(SIMULATOR)
	$(SIMULATOR)
	$(HEXTOSYX) $(HEX) $(SYX)
	sh $(TOOLS)/check_image.sh $(ELF)

# build the tool for conversion of ELF files to sysex, ready for upload to the unit
$(HEXTOSYX): | submodules
	mkdir -p $(BUILDDIR)
ifeq ($(OS),Windows_NT)
	mkdir -p $(BUILDDIR)/win_compat
	: > $(BUILDDIR)/win_compat/unistd.h
endif
	$(HOST_GPP) -O2 -std=c++14 $(HOST_PLATFORM_CFLAGS) $(HEXTOSYX_WIN_INC) -I./$(TOOLS)/libintelhex/include ./$(TOOLS)/libintelhex/src/intelhex.cc $(TOOLS)/hextosyx.cpp -o $(HEXTOSYX)

# build the simulator (it's a very basic test of the code before it runs on the device!)
$(SIMULATOR): $(SIM_SOURCES) $(wildcard include/*.h src/*/*.h)
	mkdir -p $(BUILDDIR)
	$(HOST_GCC) -g3 -O0 -std=c99 -Wall -Werror $(HOST_PLATFORM_CFLAGS) -Iinclude -Isrc $(SIM_SOURCES) -o $(SIMULATOR)

$(HEX): $(ELF)
	$(OBJCOPY) -O ihex $< $@

$(ELF): $(OBJECTS) $(LIB)
	$(LD) $(LDFLAGS) -o $@ $(OBJECTS) $(LIB)

$(LIB): $(ORIG_LIB)
	rm -rf $(LIB_WORK)
	mkdir -p $(LIB_WORK)
	cp $(ORIG_LIB) $(LIB_WORK)/trimmed.a
	$(AR) d $(LIB_WORK)/trimmed.a usb_desc.o usb_prop.o usb_endp.o
	cd $(LIB_WORK) && $(AR) x "$(CURDIR)/$(ORIG_LIB)" c_midi.o
	$(OBJCOPY) --globalize-symbol=DinRx --globalize-symbol=USBParserToDev $(LIB_WORK)/c_midi.o
	$(AR) r $(LIB_WORK)/trimmed.a $(LIB_WORK)/c_midi.o
	mv $(LIB_WORK)/trimmed.a $@

DEPENDS := $(OBJECTS:.o=.d)

-include $(DEPENDS)

$(BUILDDIR)/%.o: %.c
	mkdir -p $(dir $@)
	$(CC) -c $(CFLAGS) -MMD -o $@ $<

$(BUILDDIR)/%.o: %.cpp
	mkdir -p $(dir $@)
	$(CXX) -c $(CXXFLAGS) -MMD -o $@ $<

submodules:
	@test -f $(TOOLS)/libintelhex/include/intelhex.h || { echo "run: git submodule update --init"; exit 1; }

# original USB descriptors, extracted from the vendor archive (alt 0 must stay byte-identical)
$(GOLDEN_DESC): $(ORIG_LIB)
	mkdir -p $(BUILDDIR)/tests/ar
	cd $(BUILDDIR)/tests/ar && $(AR) x "$(CURDIR)/$(ORIG_LIB)" usb_desc.o
	$(OBJCOPY) -O binary --only-section=.rodata $(BUILDDIR)/tests/ar/usb_desc.o $@

test: $(TEST_BIN)
	$(TEST_BIN)

# the MIDI-CI responder is C++17 (Microsoft's headers); link it into the C test binary
$(TEST_CI_OBJECT): $(BUILDDIR)/tests/%.o: src/midici/%.cpp $(CI_HEADERS) src/platform/platform.h src/ump/ump.h src/instrument/settings.h
	mkdir -p $(dir $@)
	$(HOST_GPP) -std=c++17 -Wall -Werror -include src/midici/vendor/sal_shim.h -Iinclude -Isrc -c $< -o $@

$(TEST_BIN): $(GOLDEN_DESC) $(TEST_SOURCES) $(TEST_CI_OBJECT) $(wildcard tests/*.h tests/stubs/*.h include/*.h src/*/*.h)
	mkdir -p $(dir $@)
	$(HOST_GCC) $(TEST_CFLAGS) $(TEST_SOURCES) $(TEST_CI_OBJECT) -o $@

.PHONY: all elf hex simulator test submodules clean
.DELETE_ON_ERROR:

clean:
	rm -rf $(BUILDDIR)
