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
  -DENABLE_CLI=OFF
ninja
ninja install

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
  --enable-libx264 --enable-libx265 \
  --enable-encoder=libx264,libx265 \
  --enable-parser=h264,hevc \
  --enable-bsf=h264_metadata,hevc_metadata,h264_mp4toannexb,hevc_mp4toannexb \
  --extra-cflags="-I$OUT/include" \
  --extra-ldflags="-L$OUT/lib"
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

mkdir -p "$OUT/include/libavcodec"
for h in cbs cbs_h264 cbs_h265 cbs_h2645 cbs_sei h264_levels; do
  if [ -f "libavcodec/$h.h" ]; then
    cp "libavcodec/$h.h" "$OUT/include/libavcodec/"
  fi
done

echo
echo "==> Done."
echo "    Configure Sunshine with:  -DFFMPEG_PREPARED_BINARIES=$OUT"
