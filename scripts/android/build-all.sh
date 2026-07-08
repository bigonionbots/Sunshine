#!/usr/bin/env bash
#
# build-all.sh — build every native dependency Sunshine needs for Android, then print the
# exact CMake configure command for the main build.
#
# Runs build-deps.sh (OpenSSL/Opus/miniupnpc/libcurl) and build-ffmpeg.sh (FFmpeg/x264/x265)
# with a consistent ABI/API, into two prefixes under the current directory.
#
# Usage:
#   export ANDROID_NDK=/path/to/android-ndk-r26d
#   ABI=x86_64 API=29 ./scripts/android/build-all.sh
set -euo pipefail

: "${ANDROID_NDK:?set ANDROID_NDK to your NDK root (r26+ recommended)}"
ABI=${ABI:-x86_64}
API=${API:-29}
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

DEPS_PREFIX="$PWD/android-deps/$ABI"
FFMPEG_PREFIX="$PWD/ffmpeg-android/$ABI/ffmpeg"

ABI="$ABI" API="$API" PREFIX="$DEPS_PREFIX" bash "$HERE/build-deps.sh"
ABI="$ABI" API="$API" OUT="$FFMPEG_PREFIX" bash "$HERE/build-ffmpeg.sh"

cat <<EOF

==============================================================================
All Android dependencies built for ABI=$ABI.

Configure Sunshine (run from the repo root):

  PKG_CONFIG_PATH="$DEPS_PREFIX/lib/pkgconfig" \\
  cmake -B build-android -G Ninja -S . \\
    -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/android.cmake \\
    -DANDROID_ABI=$ABI \\
    -DBUILD_TESTS=OFF -DBUILD_DOCS=OFF \\
    -DFFMPEG_PREPARED_BINARIES="$FFMPEG_PREFIX" \\
    -DOPENSSL_ROOT_DIR="$DEPS_PREFIX" \\
    -DOpus_ROOT_DIR="$DEPS_PREFIX" -DOPUS_USE_STATIC=ON \\
    -DCMAKE_PREFIX_PATH="$DEPS_PREFIX" \\
    -DCMAKE_FIND_ROOT_PATH="$DEPS_PREFIX"

Then:  ninja -C build-android sunshine
==============================================================================
EOF
