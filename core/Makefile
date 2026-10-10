# PC build of the emulator core (run in WSL):  make -C core        ->  core/build/gcemu
# BUILD selects the output directory (tools/arm_build.sh uses build-arm for the ARM Linux build).
CC      ?= gcc
CFLAGS  ?= -std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter
CFLAGS  += -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
LDLIBS  := -lm
BUILD   ?= build

SRC := $(wildcard src/cpu/*.c) $(wildcard src/sys/*.c) $(wildcard src/hw/*.c) $(wildcard src/gx/*.c) $(wildcard src/boot/*.c) \
       $(wildcard src/host/*.c) $(wildcard src/disc/*.c) $(wildcard src/jit/*.c)
OBJ := $(patsubst src/%.c,$(BUILD)/obj/%.o,$(SRC))

$(BUILD)/gcemu: $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/obj/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

clean:
	rm -rf build build-arm

-include $(OBJ:.o=.d)
.PHONY: clean
