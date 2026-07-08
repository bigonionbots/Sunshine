#!/usr/bin/env bash
#
# build-ffmpeg.sh — cross-build FFmpeg + x264/x265 for a Sunshine Android build.
#
# Sunshine does not use a stock FFmpeg: src/cbs.cpp includes FFmpeg's *internal* coded
# bitstream headers (<libavcodec/cbs_h264.h>, <libavcodec/cbs_h265.h>), which a normal
# `make install` does not ship. This script therefore also packages a separate libcbs.a
# plus those internal headers, matching the layout cmake/dependencies/ffmpeg.cmake expects.
#
# Output layout (point Sunshine at $OUT via -DFFMPEG_PREPARED_BINARIES=$OUT):
#   $OUT/lib/{libavcodec,libswscale,libavutil,libcbs,libx264,libx265}.a
#   $OUT/include/...   (public FFmpeg headers + the internal cbs headers)
#
# Usage:
#   export ANDROID_NDK=/path/to/android-ndk-r26d   # r26+ recommended
#   ABI=x86_64 API=29 ./scripts/android/build-ffmpeg.sh
#   ABI=arm64-v8a ./scripts/android/build-ffmpeg.sh
#
# Prerequisites on the build host: git, cmake, ninja, make, pkg-config, nasm/yasm (x264).
set -euo pipefail

# --- Configuration ---------------------------------------------------------------------
: "${ANDROID_NDK:?set ANDROID_NDK to your NDK root (r26+ recommended)}"
ABI=${ABI:-x86_64}                              # x86_64 (matches the x86 Android VM) or arm64-v8a
API=${API:-29}                                  # min API level; 29 = Android 10
WORK=${WORK:-$PWD/ffmpeg-android-build/$ABI}    # scratch/build dir
OUT=${OUT:-$PWD/ffmpeg-android/$ABI/ffmpeg}     # install prefix consumed by Sunshine
FFMPEG_TAG=${FFMPEG_TAG:-n7.1}                  # keep in sync with the FFmpeg major Sunshine targets
JOBS=${JOBS:-$(nproc)}

# --- Map ABI -> NDK triple / FFmpeg arch ----------------------------------------------
case "$ABI" in
  x86_64)    TRIPLE=x86_64-linux-android;  FF_ARCH=x86_64 ;;
  arm64-v8a) TRIPLE=aarch64-linux-android; FF_ARCH=aarch64 ;;
  *) echo "error: unsupported ABI '$ABI' (use x86_64 or arm64-v8a)" >&2; exit 1 ;;
esac

HOST_TAG=linux-x86_64
TC="$ANDROID_NDK/toolchains/llvm/prebuilt/$HOST_TAG"
[ -d "$TC" ] || { echo "error: NDK toolchain not found at $TC" >&2; exit 1; }

export CC="$TC/bin/${TRIPLE}${API}-clang"
export CXX="$TC/bin/${TRIPLE}${API}-clang++"
export AR="$TC/bin/llvm-ar"
export RANLIB="$TC/bin/llvm-ranlib"
export STRIP="$TC/bin/llvm-strip"
export NM="$TC/bin/llvm-nm"
SYSROOT="$TC/sysroot"

mkdir -p "$WORK" "$OUT/lib" "$OUT/include"
export PKG_CONFIG_PATH="$OUT/lib/pkgconfig"
export PKG_CONFIG_LIBDIR="$OUT/lib/pkgconfig"   # keep pkg-config off the host's libs

# Assembly is enabled by default (nasm/yasm required). Set DISABLE_ASM=1 to build x264/x265
# without hand-written assembly — no nasm needed, at a real encode-performance cost. Useful for
# a bring-up spike on a host where installing nasm is inconvenient.
if [ "${DISABLE_ASM:-0}" = "1" ]; then
  X264_ASM_FLAG="--disable-asm"
  X265_ASM_FLAG="-DENABLE_ASSEMBLY=OFF"
  FFMPEG_ASM_FLAG="--disable-x86asm"
  echo "==> DISABLE_ASM=1: building x264/x265/FFmpeg without assembly (slower encode)"
else
  X264_ASM_FLAG=""
  X265_ASM_FLAG=""
  FFMPEG_ASM_FLAG=""
fi

echo "==> ABI=$ABI  API=$API  triple=$TRIPLE  out=$OUT"

# --- 1) x264 (static, PIC) -------------------------------------------------------------
cd "$WORK"
if [ ! -d x264 ]; then
  git clone --depth 1 https://code.videolan.org/videolan/x264.git
fi
cd x264
make distclean >/dev/null 2>&1 || true
./configure \
  --host="$TRIPLE" \
  --sysroot="$SYSROOT" \
  --prefix="$OUT" \
  --enable-static --enable-pic \
  --disable-cli --disable-opencl \
  ${X264_ASM_FLAG} \
  --cross-prefix="$TC/bin/llvm-"
make -j"$JOBS"
make install

# --- 2) x265 (cmake, static) -----------------------------------------------------------
cd "$WORK"
if [ ! -d x265_git ]; then
  git clone --depth 1 https://bitbucket.org/multicoreware/x265_git.git
fi
rm -rf x265_git/build/android && mkdir -p x265_git/build/android && cd x265_git/build/android
cmake -G Ninja ../../source \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI="$ABI" \
  -DANDROID_PLATFORM="android-$API" \
  -DCMAKE_INSTALL_PREFIX="$OUT" \
  -DENABLE_SHARED=OFF \
  -DENABLE_CLI=OFF \
  ${X265_ASM_FLAG}
ninja
ninja install

# x265's CMake omits x265.pc for static-only builds, so FFmpeg's pkg-config check for
# --enable-libx265 fails. Generate it. libx265 is C++, so Libs.private carries the NDK C++
# runtime (libc++_shared) — needed when FFmpeg link-tests x265 with the C compiler driver.
if [ ! -f "$OUT/lib/pkgconfig/x265.pc" ]; then
  x265_ver=$(sed -n 's/.*X265_VERSION[^0-9]*\([0-9.]\+\).*/\1/p' "$OUT/include/x265_config.h" 2>/dev/null | head -1)
  : "${x265_ver:=3.6}"
  mkdir -p "$OUT/lib/pkgconfig"
  cat > "$OUT/lib/pkgconfig/x265.pc" <<PC
prefix=$OUT
exec_prefix=\${prefix}
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: x265
Description: H.265/HEVC video encoder
Version: $x265_ver
Libs: -L\${libdir} -lx265
Libs.private: -lc++_shared -lm -ldl
Cflags: -I\${includedir}
PC
  echo "==> generated x265.pc (version $x265_ver)"
fi

# --- 3) FFmpeg (static; libx264/libx265; CBS objects via *_metadata bitstream filters) --
cd "$WORK"
if [ ! -d ffmpeg ]; then
  git clone --depth 1 --branch "$FFMPEG_TAG" https://git.ffmpeg.org/ffmpeg.git
fi
cd ffmpeg
make distclean >/dev/null 2>&1 || true
./configure \
  --prefix="$OUT" \
  --target-os=android \
  --arch="$FF_ARCH" \
  --enable-cross-compile \
  --cc="$CC" --cxx="$CXX" --ar="$AR" --ranlib="$RANLIB" --nm="$NM" --strip="$STRIP" \
  --sysroot="$SYSROOT" \
  --enable-gpl --enable-version3 \
  --enable-static --disable-shared \
  --disable-programs --disable-doc \
  --disable-avdevice --disable-avformat --disable-network \
  ${FFMPEG_ASM_FLAG} \
  --enable-libx264 --enable-libx265 \
  --enable-encoder=libx264,libx265 \
  --enable-parser=h264,hevc \
  --enable-bsf=h264_metadata,hevc_metadata,h264_mp4toannexb,hevc_mp4toannexb \
  --pkg-config-flags="--static" \
  --extra-cflags="-I$OUT/include" \
  --extra-ldflags="-L$OUT/lib" \
  --extra-libs="-lc++_shared -lm -ldl"
make -j"$JOBS"
make install   # installs libavcodec/libswscale/libavutil + their public headers

# --- 4) libcbs + internal headers (the Sunshine-specific step) --------------------------
# The cbs_h264/cbs_h265 objects are compiled into libavcodec because the *_metadata
# bitstream filters were enabled above. Archive just those objects into libcbs.a, and copy
# the INTERNAL headers that src/cbs.cpp includes (these are NOT installed by `make install`).
echo "==> packaging libcbs.a"
CBS_OBJS=()
for o in cbs cbs_h2645 cbs_sei h2645_parse; do
  if [ -f "libavcodec/$o.o" ]; then
    CBS_OBJS+=("libavcodec/$o.o")
  fi
done
if [ "${#CBS_OBJS[@]}" -eq 0 ]; then
  echo "error: no cbs objects found in libavcodec/ — inspect 'ls libavcodec/cbs*.o' and" >&2
  echo "       update the CBS_OBJS list; the object names drift between FFmpeg versions." >&2
  exit 1
fi
"$AR" rcs "$OUT/lib/libcbs.a" "${CBS_OBJS[@]}"

# src/cbs.cpp pulls a deep closure of internal libavcodec/libavutil headers
# (cbs_h264.h -> cbs_h2645.h -> h2645_parse.h -> get_bits.h -> mathops.h -> hevc/hevc.h -> ...),
# none of which `make install` ships, spread across nested subdirs (x86/, hevc/, ...). Rather
# than chase them one by one, copy both source header trees recursively (preserving structure).
for lib in libavcodec libavutil; do
  ( cd "$lib" && find . -name '*.h' -print0 | while IFS= read -r -d '' h; do
      mkdir -p "$OUT/include/$lib/$(dirname "$h")"
      cp "$h" "$OUT/include/$lib/$h"
    done )
done
# FFmpeg's generated config headers live at the build root; internal headers include them
# relatively (#include "config.h") from many subdirs. Drop a copy into every header directory.
# Kept out of include/ root so they never shadow Sunshine's own "config.h" (resolved from the
# source root via -I).
for cfg in config.h config_components.h; do
  [ -f "$cfg" ] || continue
  for lib in libavcodec libavutil; do
    find "$OUT/include/$lib" -type d -exec cp "$cfg" {} \;
  done
done

echo
echo "==> Done."
echo "    Configure Sunshine with:  -DFFMPEG_PREPARED_BINARIES=$OUT"
