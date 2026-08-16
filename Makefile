#---------------------------------------------------------------------------------
# ZOMBOTRON -- Nintendo Switch homebrew loader (wrapper port)
# Unity 6000.2.6f2 / IL2CPP / arm64-v8a. Retargeted from the badpiggies_nx /
# colorsheep_nx / laytonbmr_nx / vln_nx so-loader lineage (MIT).
#
# Ships NO game code or assets. You must supply libmain/libunity/libil2cpp/
# lib_burst_generated + the assets tree from a copy of Zombotron you legally own.
# See README.md.
#
# Requires devkitA64 + devkitPro packages:
#   dkp-pacman -S switch-mesa switch-libdrm_nouveau switch-sdl2 switch-zlib switch-libpng
#---------------------------------------------------------------------------------
.SUFFIXES:
ifeq ($(strip $(DEVKITPRO)),)
$(error "Set DEVKITPRO in your environment. (export DEVKITPRO=/opt/devkitpro)")
endif
TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

TARGET      := zombotron_nx
APP_TITLE   := Zombotron Re-Boot
APP_AUTHOR  := StevensND
APP_VERSION := 1.0.2
APP_ICON    := $(TOPDIR)/icon.jpg
export APP_TITLE APP_AUTHOR APP_VERSION APP_ICON
BUILD    := build
SOURCES  := source
INCLUDES := source

ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

CFLAGS  := -g -Wall -O2 -ffunction-sections -fdata-sections $(ARCH) $(DEFINES) \
           -DCRASH_LOG_PRINTF=stallPrintf \
           $(INCLUDE) -D__SWITCH__
# The reserved region where the game .so are loaded (keep in sync with so_util.c).
CFLAGS  += -DLOAD_ADDRESS=0xC0000000
CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++17
ASFLAGS  := -g $(ARCH)
# Route nouveau's page-aligned GPU buffers into a dedicated contiguous arena
# (source/zombotron_gpuarena.c). Without this they come out of newlib's general
# heap, fragment it, and a ~20 MB contiguous run eventually cannot be placed --
# which is the fixed-frame-count console freeze. Ported from pvz_fusion_nx.
LDFLAGS   = -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map) \
            -Wl,--build-id -Wl,--gc-sections \
            -Wl,--wrap,malloc -Wl,--wrap,calloc -Wl,--wrap,realloc \
            -Wl,--wrap,memalign -Wl,--wrap,free

# mesa GLES3 + EGL + nouveau (force the GLES path); SDL2 for window/HID/audio; zlib.
# ffmpeg backs the splash-video player (source/zombotron_video.c).
#
#   dkp-pacman -S switch-ffmpeg          (Windows/msys2: pacman -S switch-ffmpeg)
#
# ASK pkg-config RATHER THAN HARDCODING THE DEPENDENCY LIST. switch-ffmpeg is a
# STATIC build (--disable-shared --enable-static), so every one of its own
# dependencies has to appear on our link line too, and that list is a property
# of the package, not of this port: 7.1-5 wants dav1d, libass, fribidi, freetype,
# bzip2 and zlib. Hardcoding it means a package update silently breaks the link.
# --static gives the full transitive chain.
#
# The fallback below is that chain as of switch-ffmpeg 7.1-5, for the case where
# switch-pkg-config is not installed. If the link fails with undefined symbols
# from inside libavcodec, install it and let pkg-config answer instead:
#   dkp-pacman -S switch-pkg-config
# Fail early, and say what to do about it. Without this the first sign of a
# missing package is a bare "libavformat/avformat.h: No such file or directory"
# from whichever file happens to compile first, which names neither the package
# nor the opt-out. PORTLIBS is set by switch_rules, included above.
ZB_VIDEO_ON := $(shell grep -sE 'define[[:space:]]+ZB_VIDEO[[:space:]]+1' source/config.h)
ifneq ($(strip $(ZB_VIDEO_ON)),)
ifeq ($(wildcard $(PORTLIBS)/include/libavformat/avformat.h),)
$(warning ==============================================================)
$(warning  switch-ffmpeg is NOT INSTALLED, and ZB_VIDEO is 1 in config.h)
$(warning )
$(warning  looked for: $(PORTLIBS)/include/libavformat/avformat.h)
$(warning )
$(warning  Install it -- from the devkitPro msys2 shell on Windows:)
$(warning      pacman -S switch-ffmpeg)
$(warning  or on linux/macOS:)
$(warning      dkp-pacman -S switch-ffmpeg)
$(warning )
$(warning  Or build without the splash video: set ZB_VIDEO to 0 in)
$(warning  source/config.h. zombotron_video.c then compiles to empty)
$(warning  stubs needing no ffmpeg header and no ffmpeg library, and the)
$(warning  port behaves exactly as it did before the feature existed.)
$(warning ==============================================================)
$(error switch-ffmpeg missing -- see above)
endif
endif

PKGCONF     := $(DEVKITPRO)/portlibs/switch/bin/aarch64-none-elf-pkg-config

# ffmpeg is linked ONLY when ZB_VIDEO is enabled (the splash-video player). With
# ZB_VIDEO 0, zombotron_video.c compiles to empty stubs and nothing references
# ffmpeg, so we keep it off the link line entirely -- the build needs no
# switch-ffmpeg at all. (-lbz2 is an ffmpeg/freetype dep, so it rides along here.)
ifneq ($(strip $(ZB_VIDEO_ON)),)
FFMPEG_PKGS  := libavformat libavcodec libswresample libswscale libavutil
FFMPEG_LIBS  := $(shell $(PKGCONF) --static --libs $(FFMPEG_PKGS) 2>/dev/null)
ifeq ($(strip $(FFMPEG_LIBS)),)
FFMPEG_LIBS  := -lavformat -lavcodec -lswresample -lswscale -lavutil \
                -ldav1d -lass -lfribidi -lharfbuzz -lfreetype -lbz2
endif
FFMPEG_GROUP := -Wl,--start-group $(FFMPEG_LIBS) -Wl,--end-group -lbz2
else
FFMPEG_GROUP :=
endif

LIBS := -lSDL2 -lGLESv2 -lEGL -lglapi -ldrm_nouveau \
        $(FFMPEG_GROUP) \
        -lpng -lz -lnx -lm

LIBDIRS := $(PORTLIBS) $(LIBNX)

ifneq ($(BUILD),$(notdir $(CURDIR)))
export OUTPUT  := $(CURDIR)/$(TARGET)
export TOPDIR  := $(CURDIR)
export VPATH   := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))

export LD := $(CXX)
export OFILES  := $(SFILES:.s=.o) $(CPPFILES:.cpp=.o) $(CFILES:.c=.o)
export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                  -I$(PORTLIBS)/include/SDL2 -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

.PHONY: all clean
all: $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile
$(BUILD):
	@mkdir -p $@
clean:
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf
else
DEPENDS := $(OFILES:.o=.d)
NROFLAGS := --icon=$(APP_ICON) --nacp=$(OUTPUT).nacp
all : $(OUTPUT).nro
$(OUTPUT).nro : $(OUTPUT).elf $(OUTPUT).nacp
$(OUTPUT).elf : $(OFILES)
-include $(DEPENDS)
endif
