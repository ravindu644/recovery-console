BINARY  = recovery-console
OUTDIR  = output
SRCS    = main.c display.c drm.c fbdev.c term.c input.c font.c
NPROC  := $(shell nproc 2>/dev/null || echo 4)

# ── Architecture detection ───────────────────────────────────────────────
ARCH := $(shell $(CC) -dumpmachine 2>/dev/null | cut -d'-' -f1 | \
        sed 's/aarch64/aarch64/;s/x86_64/x86_64/;s/i686/x86/;s/^arm.*/armhf/')

# ── FreeType: prefer pre-built static lib, fall back to pkg-config ───────
_FT_LOCAL := deps/$(ARCH)
_FT_LIB   := $(_FT_LOCAL)/lib/libfreetype.a

ifneq ($(wildcard $(_FT_LIB)),)
  FT_CFLAGS := -I$(_FT_LOCAL)/include/freetype2 -I$(_FT_LOCAL)/include
  FT_LIBS   := $(_FT_LIB)
else
  FT_CFLAGS := $(shell pkg-config --cflags freetype2 2>/dev/null || echo "-I/usr/include/freetype2")
  FT_LIBS   := $(shell pkg-config --libs   freetype2 2>/dev/null || echo "-lfreetype")
endif

# ── Compiler flags ───────────────────────────────────────────────────────
CFLAGS  = -Wall -Wextra -O2 -std=gnu99 -no-pie
CFLAGS += -Iinclude -I.
CFLAGS += -Wshadow -Wnull-dereference -Wformat=2
CFLAGS += $(FT_CFLAGS)
# Real-time optimisations: no function descriptor indirection, tighter code.
CFLAGS += -fno-plt -ffunction-sections -fdata-sections

# ── Link flags ───────────────────────────────────────────────────────────
# -lutil: needed by glibc for forkpty().  musl includes forkpty in libc.a
# so we detect the C library and omit -lutil when building with musl.
_IS_MUSL := $(shell $(CC) -dumpmachine 2>/dev/null | grep -c musl)

ifeq ($(STATIC),1)
  LDFLAGS  = -static -no-pie -Wl,--gc-sections
  ifeq ($(_IS_MUSL),0)
    LIBS   = -lutil $(FT_LIBS)
  else
    LIBS   = $(FT_LIBS)        # musl: forkpty already in libc.a
  endif
else
  LDFLAGS  = -no-pie -Wl,--gc-sections
  ifeq ($(_IS_MUSL),0)
    LIBS   = -lutil $(FT_LIBS)
  else
    LIBS   = $(FT_LIBS)
  endif
endif

OBJDIR = $(OUTDIR)/.obj/$(ARCH)
OBJS   = $(SRCS:%.c=$(OBJDIR)/%.o)

Q := $(if $(V),,@)

# ── Cross-compiler finder ─────────────────────────────────────────────────
find-cc = $(shell \
    if [ -n "$(MUSL_CROSS)" ] && [ -f "$(MUSL_CROSS)/$(1)-gcc" ]; then \
        echo "$(MUSL_CROSS)/$(1)-gcc"; \
    elif [ -f "$(HOME)/toolchains/$(1)-cross/bin/$(1)-gcc" ]; then \
        echo "$(HOME)/toolchains/$(1)-cross/bin/$(1)-gcc"; \
    elif command -v $(1)-gcc >/dev/null 2>&1; then \
        echo "$(1)-gcc"; \
    fi)

.PHONY: all help clean native aarch64 armhf x86_64 x86 all-tarball

all: help

VERSION := $(shell grep -oP 'VERSION\s+"\K[^"]+' include/config.h 2>/dev/null || echo "v0.0.1")
TARBALL := recovery-console-$(VERSION)-$(shell date +%Y%m%d).tar.gz

help:
	@echo "Targets : native  aarch64  armhf  x86_64  x86  all-tarball  clean"
	@echo "Options : V=1 verbose | STATIC=1 full static binary"
	@echo "Env     : MUSL_CROSS=<dir> point at musl toolchain"

STRIP ?= strip

$(OBJDIR):
	@mkdir -p $@

$(OUTDIR):
	@mkdir -p $@

include/font_data.h: font.ttf
	@printf "  GEN  %s\n" $@
	$(Q)xxd -i $< > $@

$(OBJDIR)/%.o: %.c include/config.h include/display.h include/term.h \
               include/font.h include/font_data.h | $(OBJDIR)
	@printf "  CC   %s\n" $<
	$(Q)$(CC) $(CFLAGS) -DHAVE_EMBEDDED_FONT \
	    $(if $(SYSROOT),--sysroot=$(SYSROOT)) -c $< -o $@

$(BINARY): $(OBJS) | $(OUTDIR)
	@printf "  LD   %s\n" $@
	$(Q)$(CC) $(OBJS) -o $(OUTDIR)/$(BINARY)-$(ARCH) $(LDFLAGS) $(LIBS)
	$(Q)$(STRIP) $(OUTDIR)/$(BINARY)-$(ARCH) 2>/dev/null || true
	@echo "==> $(OUTDIR)/$(BINARY)-$(ARCH) (`du -h $(OUTDIR)/$(BINARY)-$(ARCH) | cut -f1`)"

# ── Convenience targets ───────────────────────────────────────────────────
native:
	$(MAKE) -j$(NPROC) $(BINARY)

aarch64:
	@CC="$(call find-cc,aarch64-linux-musl)" ; \
	[ -n "$$CC" ] || { echo "ERROR: aarch64-linux-musl-gcc not found"; exit 1; } ; \
	$(MAKE) -j$(NPROC) $(BINARY) CC=$$CC STRIP=$${CC%gcc}strip STATIC=1

armhf:
	@CC="$(call find-cc,arm-linux-musleabihf)" ; \
	[ -n "$$CC" ] || CC="$(call find-cc,armv7l-linux-musleabihf)" ; \
	[ -n "$$CC" ] || { echo "ERROR: arm-linux-musleabihf-gcc not found"; exit 1; } ; \
	$(MAKE) -j$(NPROC) $(BINARY) CC=$$CC STRIP=$${CC%gcc}strip STATIC=1

x86_64:
	@CC="$(call find-cc,x86_64-linux-musl)" ; \
	[ -n "$$CC" ] || { echo "ERROR: x86_64-linux-musl-gcc not found"; exit 1; } ; \
	$(MAKE) -j$(NPROC) $(BINARY) CC=$$CC STRIP=$${CC%gcc}strip STATIC=1

x86:
	@CC="$(call find-cc,i686-linux-musl)" ; \
	[ -n "$$CC" ] || { echo "ERROR: i686-linux-musl-gcc not found"; exit 1; } ; \
	$(MAKE) -j$(NPROC) $(BINARY) CC=$$CC STRIP=$${CC%gcc}strip STATIC=1

# ── Dependency build target ───────────────────────────────────────────────
build-deps:
	@echo "[*] Building dependencies for all architectures..."
	$(Q)./scripts/build-freetype.sh aarch64-linux-musl
	$(Q)./scripts/build-freetype.sh arm-linux-musleabihf
	$(Q)./scripts/build-freetype.sh x86_64-linux-musl
	$(Q)./scripts/build-freetype.sh i686-linux-musl

# ── CI/Release target ─────────────────────────────────────────────────────
all-tarball: aarch64 armhf x86_64 x86
	@printf "  TAR  %s\n" $(TARBALL)
	$(Q)tar -czf $(TARBALL) -C $(OUTDIR) .
	@echo "==> $(TARBALL)"

clean:
	@rm -rf $(OUTDIR) *.tar.gz && echo "cleaned"
