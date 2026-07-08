#!/usr/bin/env bash
#
# build-deps.sh — cross-build Sunshine's remaining native dependencies for Android.
#
# Covers the libraries that CMake cannot build itself from the Sunshine tree:
#   OpenSSL, Opus, miniupnpc, libcurl (curl links against the OpenSSL built here).
#
# NOT built here (handled elsewhere):
#   - FFmpeg / x264 / x265         -> scripts/android/build-ffmpeg.sh
#   - Boost                        -> built automatically by Sunshine's CMake FetchContent
#                                     fallback using the NDK toolchain (see Boost_Sunshine.cmake)
#   - moonlight-common-c / enet, Simple-Web-Server, nlohmann_json, libdisplaydevice
#                                  -> built from source as CMake subprojects during the Sunshine build
#
# Everything installs into a single prefix ($PREFIX) so one set of root vars / PKG_CONFIG_PATH
# covers them all when configuring Sunshine.
#
# Usage:
#   export ANDROID_NDK=/path/to/android-ndk-r26d
#   ABI=x86_64 API=29 ./scripts/android/build-deps.sh
#   ABI=arm64-v8a ./scripts/android/build-deps.sh
#
# Prerequisites on the build host: git, cmake, ninja, make, perl (for OpenSSL), pkg-config.
set -euo pipefail

# --- Configuration ---------------------------------------------------------------------
: "${ANDROID_NDK:?set ANDROID_NDK to your NDK root (r26+ recommended)}"
ABI=${ABI:-x86_64}                                # x86_64 or arm64-v8a
API=${API:-29}
WORK=${WORK:-$PWD/android-deps-build/$ABI}        # scratch/build dir
PREFIX=${PREFIX:-$PWD/android-deps/$ABI}          # shared install prefix consumed by Sunshine
JOBS=${JOBS:-$(nproc)}

# Pin versions; override via env as needed.
OPENSSL_TAG=${OPENSSL_TAG:-openssl-3.3.2}
OPUS_TAG=${OPUS_TAG:-v1.5.2}
MINIUPNP_TAG=${MINIUPNP_TAG:-miniupnpc_2_2_8}
CURL_TAG=${CURL_TAG:-curl-8_11_1}

# --- Map ABI -> NDK triple / OpenSSL target -------------------------------------------
case "$ABI" in
  x86_64)    TRIPLE=x86_64-linux-android;  SSL_TARGET=android-x86_64 ;;
  arm64-v8a) TRIPLE=aarch64-linux-android; SSL_TARGET=android-arm64 ;;
  *) echo "error: unsupported ABI '$ABI' (use x86_64 or arm64-v8a)" >&2; exit 1 ;;
esac

HOST_TAG=linux-x86_64
TC="$ANDROID_NDK/toolchains/llvm/prebuilt/$HOST_TAG"
[ -d "$TC" ] || { echo "error: NDK toolchain not found at $TC" >&2; exit 1; }
NDK_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake"

mkdir -p "$WORK" "$PREFIX"
echo "==> ABI=$ABI  API=$API  prefix=$PREFIX"

# --- Helper: configure/build/install a CMake-based dependency --------------------------
# Usage: cmake_dep <name> <srcdir> [extra -D args...]
cmake_dep() {
  local name=$1 src=$2
  shift 2
  echo "==> building $name"
  cmake -G Ninja -S "$src" -B "$src/build-android" \
    -DCMAKE_TOOLCHAIN_FILE="$NDK_TOOLCHAIN_FILE" \
    -DANDROID_ABI="$ABI" \
    -DANDROID_PLATFORM="android-$API" \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_FIND_ROOT_PATH="$PREFIX" \
    -DBUILD_SHARED_LIBS=OFF \
    "$@"
  ninja -C "$src/build-android"
  ninja -C "$src/build-android" install
}

# --- 1) OpenSSL (autotools-style Configure with built-in Android targets) --------------
# Must be first: libcurl links against it. OpenSSL finds the NDK clang via the target name
# plus ANDROID_NDK_ROOT and the toolchain bin on PATH.
echo "==> building OpenSSL ($SSL_TARGET)"
export ANDROID_NDK_ROOT="$ANDROID_NDK"
export PATH="$TC/bin:$PATH"
cd "$WORK"
[ -d openssl ] || git clone --depth 1 --branch "$OPENSSL_TAG" https://github.com/openssl/openssl.git
cd openssl
./Configure "$SSL_TARGET" -D__ANDROID_API__="$API" \
  no-shared no-tests no-apps \
  --prefix="$PREFIX" --libdir=lib --openssldir="$PREFIX/ssl"
make -j"$JOBS"
make install_sw       # libraries + headers, skip docs

# --- 2) Opus (CMake; static) -----------------------------------------------------------
cd "$WORK"
[ -d opus ] || git clone --depth 1 --branch "$OPUS_TAG" https://github.com/xiph/opus.git
cmake_dep opus "$WORK/opus" \
  -DOPUS_BUILD_SHARED_LIBRARY=OFF \
  -DOPUS_BUILD_PROGRAMS=OFF \
  -DOPUS_BUILD_TESTING=OFF

# --- 3) miniupnpc (CMake; static) ------------------------------------------------------
cd "$WORK"
[ -d miniupnp ] || git clone --depth 1 --branch "$MINIUPNP_TAG" https://github.com/miniupnp/miniupnp.git
cmake_dep miniupnpc "$WORK/miniupnp/miniupnpc" \
  -DUPNPC_BUILD_SHARED=OFF \
  -DUPNPC_BUILD_TESTS=OFF \
  -DUPNPC_BUILD_SAMPLE=OFF

# --- 4) libcurl (CMake; static; OpenSSL backend from step 1) ---------------------------
cd "$WORK"
[ -d curl ] || git clone --depth 1 --branch "$CURL_TAG" https://github.com/curl/curl.git
cmake_dep curl "$WORK/curl" \
  -DBUILD_CURL_EXE=OFF \
  -DBUILD_TESTING=OFF \
  -DCURL_USE_OPENSSL=ON \
  -DOPENSSL_ROOT_DIR="$PREFIX" \
  -DCURL_USE_LIBPSL=OFF \
  -DCURL_DISABLE_LDAP=ON \
  -DCURL_DISABLE_LDAPS=ON

echo
echo "==> Done. Dependency prefix: $PREFIX"
echo "    Pass to Sunshine:  -DOPENSSL_ROOT_DIR=$PREFIX \\"
echo "                       -DOpus_ROOT_DIR=$PREFIX -DOPUS_USE_STATIC=ON \\"
echo "                       -DCMAKE_PREFIX_PATH=$PREFIX -DCMAKE_FIND_ROOT_PATH=$PREFIX"
echo "    And export:        PKG_CONFIG_PATH=$PREFIX/lib/pkgconfig  (for miniupnpc + libcurl)"
