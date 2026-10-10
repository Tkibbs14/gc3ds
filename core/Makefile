# PC build of the emulator core (run in WSL):  make -C core        ->  core/build/gcemu
CC      ?= gcc
CFLAGS  ?= -std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter
CFLAGS  += -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
LDLIBS  := -lm

SRC := $(wildcard src/cpu/*.c) $(wildcard src/sys/*.c) $(wildcard src/hw/*.c) $(wildcard src/gx/*.c) $(wildcard src/boot/*.c) \
       $(wildcard src/host/*.c)
OBJ := $(patsubst src/%.c,build/obj/%.o,$(SRC))

build/gcemu: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

build/obj/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

clean:
	rm -rf build

-include $(OBJ:.o=.d)
.PHONY: clean
