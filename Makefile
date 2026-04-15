BINARY  = recovery-console
OUTDIR  = output
SRCS    = main.c display.c drm.c fbdev.c term.c input.c font.c
NPROC  := $(shell nproc 2>/dev/null || echo 4)

# Arch (set by sub-make calls below; auto-detected from CC otherwise)
ARCH := $(shell $(CC) -dumpmachine 2>/dev/null | cut -d'-' -f1 | \
        sed 's/aarch64/aarch64/;s/x86_64/x86_64/;s/i686/x86/;s/^arm.*/armhf/')

# Local FreeType: built by scripts/build-freetype.sh into deps/<arch>/
_FT_LOCAL := deps/$(ARCH)
_FT_LIB   := $(_FT_LOCAL)/lib/libfreetype.a

ifneq ($(wildcard $(_FT_LIB)),)
  FT_CFLAGS := -I$(_FT_LOCAL)/include/freetype2 -I$(_FT_LOCAL)/include
  FT_LIBS   := $(_FT_LIB)
else
  FT_CFLAGS := $(shell pkg-config --cflags freetype2 2>/dev/null || echo "-I/usr/include/freetype2")
  FT_LIBS   := $(shell pkg-config --libs   freetype2 2>/dev/null || echo "-lfreetype")
endif

CFLAGS  = -Wall -Wextra -O2 -std=gnu99 -no-pie -Iinclude -I.
CFLAGS += -Wshadow -Wnull-dereference -Wformat=2
CFLAGS += $(FT_CFLAGS)

# STATIC=1 links everything statically
ifeq ($(STATIC),1)
LDFLAGS = -static -no-pie
LIBS    = -lutil $(FT_LIBS)
else
LDFLAGS = -no-pie
LIBS    = -lutil $(FT_LIBS)
endif

OBJDIR = $(OUTDIR)/.obj/$(ARCH)
OBJS   = $(SRCS:%.c=$(OBJDIR)/%.o)

Q := $(if $(V),,@)

find-cc = $(shell \
    if [ -n "$(MUSL_CROSS)" ] && [ -f "$(MUSL_CROSS)/$(1)-gcc" ]; then \
        echo "$(MUSL_CROSS)/$(1)-gcc"; \
    elif [ -f "$(HOME)/toolchains/$(1)-cross/bin/$(1)-gcc" ]; then \
        echo "$(HOME)/toolchains/$(1)-cross/bin/$(1)-gcc"; \
    elif command -v $(1)-gcc >/dev/null 2>&1; then \
        echo "$(1)-gcc"; \
    fi)

.PHONY: all help clean native aarch64 armhf x86_64 x86

all: help

help:
	@echo "Targets: native  aarch64  armhf  x86_64  x86  clean"
	@echo "Options: V=1 verbose | STATIC=1 full static"

STRIP ?= strip

$(OBJDIR):
	@mkdir -p $@

$(OUTDIR):
	@mkdir -p $@

include/font_data.h: font.ttf
	@printf "  GEN %s\n" $@
	$(Q)xxd -i $< > $@

$(OBJDIR)/%.o: %.c include/config.h include/display.h include/term.h include/font.h include/font_data.h | $(OBJDIR)
	@printf "  CC  %s\n" $<
	$(Q)$(CC) $(CFLAGS) -DHAVE_EMBEDDED_FONT $(if $(SYSROOT),--sysroot=$(SYSROOT)) -c $< -o $@

$(BINARY): $(OBJS) | $(OUTDIR)
	@printf "  LD  %s\n" $@
	$(Q)$(CC) $(OBJS) -o $(OUTDIR)/$(BINARY)-$(ARCH) $(LDFLAGS) $(LIBS)
	$(Q)$(STRIP) $(OUTDIR)/$(BINARY)-$(ARCH) 2>/dev/null || true
	@echo "==> $(OUTDIR)/$(BINARY)-$(ARCH) (`du -h $(OUTDIR)/$(BINARY)-$(ARCH) | cut -f1`)"

native:
	$(MAKE) -j$(NPROC) $(BINARY)

aarch64:
	@CC="$(call find-cc,aarch64-linux-musl)" ; \
	[ -n "$$CC" ] || { echo "aarch64-linux-musl-gcc not found"; exit 1; } ; \
	$(MAKE) -j$(NPROC) $(BINARY) CC=$$CC STRIP=$${CC%gcc}strip STATIC=1

armhf:
	@CC="$(call find-cc,arm-linux-musleabihf)" ; \
	[ -n "$$CC" ] || CC="$(call find-cc,armv7l-linux-musleabihf)" ; \
	[ -n "$$CC" ] || { echo "arm-linux-musleabihf-gcc not found"; exit 1; } ; \
	$(MAKE) -j$(NPROC) $(BINARY) CC=$$CC STRIP=$${CC%gcc}strip STATIC=1

x86_64:
	@CC="$(call find-cc,x86_64-linux-musl)" ; \
	[ -n "$$CC" ] || { echo "x86_64-linux-musl-gcc not found"; exit 1; } ; \
	$(MAKE) -j$(NPROC) $(BINARY) CC=$$CC STRIP=$${CC%gcc}strip STATIC=1

x86:
	@CC="$(call find-cc,i686-linux-musl)" ; \
	[ -n "$$CC" ] || { echo "i686-linux-musl-gcc not found"; exit 1; } ; \
	$(MAKE) -j$(NPROC) $(BINARY) CC=$$CC STRIP=$${CC%gcc}strip STATIC=1

clean:
	@rm -rf $(OUTDIR) && echo "cleaned"
