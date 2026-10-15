# gc3ds native mode: Twilight Princess from the Dusklight source (github.com/TwilitRealm/dusk, CC0) built for the 3DS's
# ARM11. Plan and status: docs/research/NATIVE_PORT_PLAN.md. The Dusklight checkout lives outside this repository
# (DUSK, at the commit in patches/dusk.base, with patches/dusk.patch applied); no game data is ever built in.
#   make -f core/native/Makefile objs       compile the game's sources to objects
#   make -f core/native/Makefile size       code and data size of what was compiled
DUSK     ?= /root/ref/dusk
OUT      ?= /root/native/build
DEPS     ?= /root/ref/deps
DEVKITARM ?= /opt/devkitpro/devkitARM
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
            -I$(DUSK)/extern -I$(DUSK)/extern/borealis/include -I$(DEPS)/fmt/include -I$(DEPS)/json/include -I$(DEPS)/cxxopts/include
# As Dusklight's GCC flags (CMakeLists.txt), plus int-sized enums (devkitARM defaults to short ones; the game asserts).
COMMON   := $(ARCH) $(OPT) -ffunction-sections -fdata-sections -fsigned-char -fno-short-enums -Wno-multichar -w $(DEFS) $(INCS)
CFLAGS   := -std=gnu11 $(COMMON)
CXXFLAGS := -std=gnu++20 -Wno-deprecated-declarations -include native_prefix.h $(COMMON)

# The game's sources as files.cmake names them, without Dusklight's own platform layer (src/dusk).
SRCS     := $(shell grep -oE "^[[:space:]]+(src|libs)/[^[:space:]]+\.(c|cpp)" $(DUSK)/files.cmake | tr -d ' \t' | grep -v "^src/dusk/" | sort -u)
OBJS     := $(patsubst %,$(OUT)/obj/%.o,$(SRCS))

.PHONY: objs size
objs: $(OBJS)
	@echo "compiled $(words $(OBJS)) objects into $(OUT)/obj"

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
