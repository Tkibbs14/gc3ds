# gc3ds native mode: Twilight Princess from the Dusklight source (github.com/TwilitRealm/dusk, CC0) built for the 3DS's
# ARM11. Plan and status: docs/research/NATIVE_PORT_PLAN.md. The Dusklight checkout lives outside this repository
# (DUSK, at the commit in patches/dusk.base, with patches/dusk.patch applied); no game data is ever built in.
#   make -f core/native/Makefile objs       compile the game's sources to objects
#   make -f core/native/Makefile size       code and data size of what was compiled
DUSK     ?= /root/ref/dusk
OUT      ?= /root/native/build
DEPS     ?= /root/ref/deps
DEVKITPRO ?= /opt/devkitpro
DEVKITARM ?= $(DEVKITPRO)/devkitARM
NATIVE   := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))
CC       := $(DEVKITARM)/bin/arm-none-eabi-gcc
CXX      := $(DEVKITARM)/bin/arm-none-eabi-g++
SIZE     := $(DEVKITARM)/bin/arm-none-eabi-size

ARCH     := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
OPT      ?= -O2
DEFS     := -D__3DS__ -DDUSK_BUILDING_GAME=1 -DTARGET_PC=1 -DWIDESCREEN_SUPPORT=1 -DAVOID_UB=1 -DVERSION=0 -DMTX_USE_PS=1 \
            -DPARTIAL_DEBUG=1 -DNDEBUG=1 -DNDEBUG_DEFINED=1 -DDEBUG_DEFINED=0 -DDUSK_HAS_FUNCHOOK=0 -DDUSK_HAS_PREPATCH=0 -DFMT_HEADER_ONLY=1
INCS     := -I$(abspath $(NATIVE))/shim -I$(DUSK)/include -I$(DUSK)/assets/GZ2E01 -I$(DUSK)/libs/JSystem/include \
            -I$(DUSK)/extern/aurora/include/dolphin -I$(DUSK)/extern/aurora/include -I$(DUSK)/sdk/include -I$(DUSK)/src \
            -I$(DUSK)/extern -I$(DUSK)/extern/borealis/include -I$(DEPS)/fmt/include -I$(DEPS)/json/include -I$(DEPS)/cxxopts/include -I$(DEPS)/xxhash -I$(DEPS)/imgui -I$(DEPS)/miniz -I$(DEPS)/picosha2
# As Dusklight's GCC flags (CMakeLists.txt), plus int-sized enums (devkitARM defaults to short ones; the game asserts).
# -MMD -MP: a .d per object, included below, so a header change rebuilds what uses it (DUSK_AUDIO_SKIP in
# dusk/audio.h did not, 2026-10-15: the play scene kept waiting on audio).
COMMON   := -MMD -MP $(ARCH) $(OPT) -ffunction-sections -fdata-sections -fsigned-char -fno-short-enums -Wno-multichar -w $(DEFS) $(INCS)
CFLAGS   := -std=gnu11 $(COMMON)
CXXFLAGS := -std=gnu++20 -Wno-deprecated-declarations -include native_prefix.h $(COMMON)

# The game's sources as files.cmake names them, without Dusklight's own platform layer (src/dusk).
SRCS     := $(shell grep -oE "^[[:space:]]+(src|libs)/[^[:space:]]+\.(c|cpp)" $(DUSK)/files.cmake | tr -d ' \t' | grep -v "^src/dusk/" | grep -vx "src/m_Do/m_Do_main.cpp" | sort -u)
OBJS     := $(patsubst %,$(OUT)/obj/%.o,$(SRCS))
# Dusklight's own layer (src/dusk), without its PC-only parts: mods, ImGui, RmlUi menus, Discord / Sentry, and the
# SDL mouse / gamepad / touch-pointer code (the 3DS platform layer provides input).
# The mod service layer is kept: it also implements the game's item-check, message-flow, stage and save hooks, which
# with no mods registered behave as the original game. Its loader, packaging, networking and UI services are not.
DUSK_EXCL := /imgui/ /ui/ discord sentry mouse.cpp gamepad_color.cpp action_bindings.cpp menu_pointer.cpp \
             /mods/loader/ mods/manifest.cpp mods/updates.cpp svc/http svc/net svc/websocket svc/ui.cpp \
             svc/overlay svc/window svc/gfx svc/texture svc/host svc/file
DSRCS    := $(shell grep -oE "^[[:space:]]+src/dusk/[^[:space:]]+\.(c|cpp)" $(DUSK)/files.cmake | tr -d ' \t' | grep -vE "$(subst $(eval) ,|,$(strip $(DUSK_EXCL)))" | sort -u)
# Dusklight's audio layer (GAME_DEBUG_FILES in its CMakeLists.txt, not in files.cmake). DuskAudioSystem (SDL audio
# output) and DuskDsp (the DSP mixer, freeverb) come with the audio step; the feasibility build boots without audio.
DSRCS    += src/dusk/audio/JASCriticalSection.cpp src/dusk/audio/Adpcm.cpp src/dusk/audio/DspStub.cpp
DOBJS    := $(patsubst %,$(OUT)/obj/%.o,$(DSRCS))
# Aurora's portable SDK layer: OS, matrices, display lists, VI, THP, ARAM and the GX API (which encodes GX FIFO
# commands; the 3DS consumes them with gc3ds's GX pipeline, not Aurora's WebGPU renderer). DVD, pad, mouse, card,
# serial and Aurora's own start-up / window / threads come from the 3DS platform layer instead.
AUR      := $(DUSK)/extern/aurora
ASRCS    := $(filter-out $(AUR)/lib/dolphin/si/si.cpp, \
              $(wildcard $(AUR)/lib/dolphin/*.cpp $(AUR)/lib/dolphin/os/*.cpp $(AUR)/lib/dolphin/mtx/*.cpp \
                         $(AUR)/lib/dolphin/gd/*.cpp $(AUR)/lib/dolphin/gx/*.cpp $(AUR)/lib/dolphin/vi/*.cpp \
                         $(AUR)/lib/dolphin/thp/*.cpp) $(AUR)/lib/logging.cpp $(AUR)/lib/time.cpp \
              $(AUR)/lib/gx/dl.cpp $(AUR)/lib/gx/fifo.cpp $(AUR)/lib/gx/attr_fmt.cpp \
              $(AUR)/lib/dolphin/dvd/dvd.cpp $(AUR)/lib/dolphin/dvd/fst.cpp $(AUR)/lib/io.cpp \
              $(wildcard $(AUR)/lib/card/*.cpp))
AOBJS    := $(patsubst $(AUR)/%.cpp,$(OUT)/aurora/%.o,$(ASRCS))
# The matrix library is C (C_MTX*, C_VEC*, C_QUAT*).
ACSRCS   := $(wildcard $(AUR)/lib/dolphin/mtx/*.c)
AOBJS    += $(patsubst $(AUR)/%.c,$(OUT)/aurora/%.c.o,$(ACSRCS))
AFLAGS   := -std=gnu++20 -MMD -MP $(ARCH) $(OPT) -ffunction-sections -fdata-sections -fsigned-char -fno-short-enums -w \
            -D__3DS__ -DAURORA -DTARGET_PC -DAURORA_ENABLE_GX -DMTX_USE_PS=1 -DFMT_HEADER_ONLY=1 \
            -I$(abspath $(NATIVE))/shim -I$(DEPS)/fmt/include -I$(DEPS)/xxhash -I$(DEPS)/imgui \
            -I$(AUR)/include -I$(AUR)/include/dolphin -I$(AUR)/lib -I$(DUSK)/extern/borealis/include
# Our replacement for Dusklight's PC entry point (m_Do_main.cpp), and the platform layer.
PLATFORM := $(wildcard $(NATIVE)/platform/*.cpp)
POBJS    := $(patsubst $(NATIVE)/platform/%.cpp,$(OUT)/platform/%.o,$(PLATFORM))
# The platform's Aurora half (start-up, frame calls, the FIFO consumer), with Aurora's flags and internal headers.
APLAT    := $(wildcard $(NATIVE)/platform/aurora/*.cpp)
POBJS    += $(patsubst $(NATIVE)/platform/aurora/%.cpp,$(OUT)/platform/aurora/%.o,$(APLAT))

.PHONY: objs size
objs: $(OBJS) $(DOBJS) $(AOBJS) $(POBJS)
	@echo "compiled $(words $(OBJS)) game, $(words $(DOBJS)) Dusklight-layer, $(words $(AOBJS)) Aurora and $(words $(POBJS)) platform objects into $(OUT)"

$(OUT)/aurora/%.o: $(AUR)/%.cpp
	@mkdir -p $(dir $@)
	@$(CXX) $(AFLAGS) -c $< -o $@ 2> $@.err || { echo "FAIL aurora/$*.cpp"; rm -f $@; }
	@[ -s $@.err ] || rm -f $@.err

$(OUT)/obj/%.cpp.o: $(DUSK)/%.cpp
	@mkdir -p $(dir $@)
	@$(CXX) $(CXXFLAGS) -c $< -o $@ 2> $@.err || { echo "FAIL $*.cpp"; rm -f $@; }
	@[ -s $@.err ] || rm -f $@.err
$(OUT)/obj/%.c.o: $(DUSK)/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c $< -o $@ 2> $@.err || { echo "FAIL $*.c"; rm -f $@; }
	@[ -s $@.err ] || rm -f $@.err

size:
	@find $(OUT)/obj -name '*.o' | xargs $(SIZE) -t | tail -1

$(OUT)/platform/%.o: $(NATIVE)/platform/%.cpp
	@mkdir -p $(dir $@)
	@$(CXX) $(CXXFLAGS) -I$(NATIVE)/platform -I$(DEVKITPRO)/libctru/include -c $< -o $@ 2> $@.err || { echo "FAIL platform/$*.cpp"; rm -f $@; }
	@[ -s $@.err ] || rm -f $@.err
$(OUT)/aurora/%.c.o: $(AUR)/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(filter-out -std=gnu++20,$(AFLAGS)) -std=gnu11 -c $< -o $@ 2> $@.err || { echo "FAIL aurora/$*.c"; rm -f $@; }
	@[ -s $@.err ] || rm -f $@.err
$(OUT)/platform/aurora/%.o: $(NATIVE)/platform/aurora/%.cpp
	@mkdir -p $(dir $@)
	@$(CXX) $(AFLAGS) -I$(DEVKITPRO)/libctru/include -c $< -o $@ 2> $@.err || { echo "FAIL platform/aurora/$*.cpp"; rm -f $@; }
	@[ -s $@.err ] || rm -f $@.err

-include $(shell find $(OUT) -name "*.d" 2>/dev/null)
