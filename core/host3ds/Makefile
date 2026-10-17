#---------------------------------------------------------------------------------
# gc3ds for the 3DS (devkitARM). Run in WSL:  bash core/host3ds/build.sh   ->  core/host3ds/gc3ds.3dsx
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITARM)),)
$(error "Set DEVKITARM: export DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

TARGET      := gc3ds
BUILD       := build
SOURCES     := . ../src/cpu ../src/aotgen ../src/sys ../src/hw ../src/gx ../src/gx/pica ../src/boot ../src/disc ../src/jit ../tests/jit
DATA        :=
INCLUDES    := .
ROMFS       := romfs

APP_TITLE       := gc3ds
APP_DESCRIPTION := GameCube emulator (development)
APP_AUTHOR      := gc3ds contributors

ARCH     := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CFLAGS   := -g -Wall -Wextra -Wno-unused-parameter -O2 -mword-relocations -ffunction-sections $(ARCH)
CFLAGS   += $(INCLUDE) -D__3DS__ -std=gnu11
CFLAGS   += $(EXTRA_CFLAGS)  # e.g. EXTRA_CFLAGS=-DGC_SWAPRAM=1 BUILD=build-swap (that layout's objects apart)
ASFLAGS  := -g $(ARCH)
LDFLAGS   = -specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)
LIBS     := -lcitro3d -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lz -lctru -lm
LIBDIRS  := $(CTRULIB) $(PORTLIBS)

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT   := $(CURDIR)/$(TARGET)
export TOPDIR   := $(CURDIR)
export VPATH    := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR  := $(CURDIR)/$(BUILD)

CFILES      := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
PICAFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.v.pica)))
export LD   := $(CC)

export OFILES_SOURCES := $(CFILES:.c=.o)
export OFILES_BIN     := $(PICAFILES:.v.pica=.shbin.o)
export OFILES         := $(OFILES_BIN) $(OFILES_SOURCES)
export HFILES         := $(PICAFILES:.v.pica=_shbin.h)

export INCLUDE  := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) $(foreach dir,$(LIBDIRS),-I$(dir)/include) -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export APP_ICON := $(CTRULIB)/default_icon.png
export _3DSXFLAGS += --smdh=$(CURDIR)/$(TARGET).smdh
export _3DSXFLAGS += --romfs=$(CURDIR)/$(ROMFS)

.PHONY: all clean

all: $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

$(BUILD):
	@mkdir -p $@

clean:
	@rm -fr $(BUILD) $(TARGET).3dsx $(TARGET).smdh $(TARGET).elf

#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------

$(OUTPUT).3dsx: $(OUTPUT).elf $(OUTPUT).smdh
$(OUTPUT).elf: $(OFILES)
$(OFILES_SOURCES): $(HFILES)
# Ahead-of-time translated code (src/aotgen): large and never stepped through, so no debug info.
aot_gen%.o: CFLAGS += -g0 -fwrapv

%.shbin.o %_shbin.h : %.v.pica
	@echo $(notdir $<)
	$(eval CURBIN := $*.shbin)
	picasso -o $(CURBIN) $<
	bin2s $(CURBIN) | $(AS) -o $*.shbin.o
	echo "extern const u8 $(subst .,_,$(CURBIN))_end[];" > $(subst .,_,$(CURBIN)).h
	echo "extern const u8 $(subst .,_,$(CURBIN))[];" >> $(subst .,_,$(CURBIN)).h
	echo "extern const u32 $(subst .,_,$(CURBIN))_size;" >> $(subst .,_,$(CURBIN)).h

-include $(DEPSDIR)/*.d

#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------
