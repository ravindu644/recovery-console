#!/bin/sh
# Cross-compile a minimal FreeType 2 static lib for a musl target.
# Usage:
#   ./scripts/build-freetype.sh aarch64-linux-musl
#   ./scripts/build-freetype.sh arm-linux-musleabihf
#   ./scripts/build-freetype.sh x86_64-linux-musl
#   ./scripts/build-freetype.sh i686-linux-musl
#
# Output: deps/<arch>/include/  deps/<arch>/lib/libfreetype.a
#
# Requires: wget/curl, tar, make, cmake or autoconf (uses cmake if available)

set -e

TRIPLE="${1:-aarch64-linux-musl}"
ARCH=$(echo "$TRIPLE" | cut -d- -f1 | sed 's/arm.*/armhf/;s/i686/x86/')
FT_VER="2.13.3"
FT_URL="https://download.savannah.gnu.org/releases/freetype/freetype-${FT_VER}.tar.gz"
DESTDIR="$(cd "$(dirname "$0")/.." && pwd)/deps/${ARCH}"
BUILD="/tmp/ft-build-${ARCH}"

# Locate the cross-compiler
CC_BIN=""
for d in "$HOME/toolchains/${TRIPLE}-cross/bin" "/usr/bin"; do
  if [ -f "$d/${TRIPLE}-gcc" ]; then CC_BIN="$d"; break; fi
done
if [ -z "$CC_BIN" ]; then echo "Compiler ${TRIPLE}-gcc not found."; exit 1; fi
CROSS_CC="$CC_BIN/${TRIPLE}-gcc"
CROSS_AR="$(command -v ar)"
CROSS_RANLIB="$(command -v ranlib)"

echo "==> Building FreeType ${FT_VER} for ${TRIPLE}"
echo "    Compiler : ${CROSS_CC}"
echo "    Output   : ${DESTDIR}"

mkdir -p "$BUILD" "$DESTDIR"

# Download + extract
if [ ! -f "$BUILD/freetype-${FT_VER}.tar.gz" ]; then
  if command -v wget >/dev/null 2>&1; then
    wget -q -O "$BUILD/freetype-${FT_VER}.tar.gz" "$FT_URL"
  else
    curl -L -o "$BUILD/freetype-${FT_VER}.tar.gz" "$FT_URL"
  fi
fi

cd "$BUILD"
tar xf "freetype-${FT_VER}.tar.gz"
SRCDIR="$BUILD/freetype-${FT_VER}"

# Use --without-xxx to keep it tiny: no HarfBuzz, no PNG, no bzip2, no brotli.
mkdir -p "$BUILD/build"
cd "$BUILD/build"

"$SRCDIR/configure" \
  --host="$TRIPLE" \
  --prefix="$DESTDIR" \
  --enable-static \
  --disable-shared \
  --without-harfbuzz \
  --without-png \
  --without-bzip2 \
  --without-brotli \
  --without-zlib \
  CC="$CROSS_CC" \
  AR="$CROSS_AR" \
  RANLIB="$CROSS_RANLIB" \
  CFLAGS="-O2 -fPIC"

make -j"$(nproc 2>/dev/null || echo 4)"
make install

echo ""
echo "==> Done! Headers and library installed under:"
echo "    ${DESTDIR}/include/"
echo "    ${DESTDIR}/lib/libfreetype.a"
echo ""
echo "Build with: make aarch64  (Makefile will auto-discover deps/)"
