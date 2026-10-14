# PC build of the emulator core (run in WSL):  make -C core        ->  core/build/gcemu
# BUILD selects the output directory (tools/arm_build.sh uses build-arm for the ARM Linux build).
CC      ?= gcc
CFLAGS  ?= -std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter
CFLAGS  += -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
LDLIBS  := -lm
BUILD   ?= build

SRC := $(wildcard src/cpu/*.c) $(wildcard src/sys/*.c) $(wildcard src/hw/*.c) $(wildcard src/gx/*.c) $(wildcard src/boot/*.c) \
       $(wildcard src/host/*.c) $(wildcard src/disc/*.c) $(wildcard src/jit/*.c) $(wildcard src/aotgen/*.c)
OBJ := $(patsubst src/%.c,$(BUILD)/obj/%.o,$(SRC))

$(BUILD)/gcemu: $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# Ahead-of-time translated code (src/aotgen, generated from the user's own disc, never committed): check-mode store log
# on, and no contracted multiply-adds (the 3DS's VFP has none).
AOT_LOG ?= -DAOT_LOG
$(BUILD)/obj/aotgen/%.o: override CFLAGS += $(AOT_LOG) -ffp-contract=off -fwrapv $(AOT_PROF)
$(BUILD)/obj/cpu/aot.o: override CFLAGS += $(AOT_PROF)

$(BUILD)/obj/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

clean:
	rm -rf build build-arm

-include $(OBJ:.o=.d)
.PHONY: clean
