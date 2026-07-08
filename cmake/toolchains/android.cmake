# Android (NDK) cross-compilation toolchain for Sunshine.
#
# This is a thin convenience wrapper around the NDK's own toolchain file. It sets project
# defaults (ABI/API level/STL) and then chains to $ANDROID_NDK/build/cmake/android.toolchain.cmake,
# which does the real work of pointing CMake at the NDK clang, sysroot, and target triple.
#
# Usage:
#   export ANDROID_NDK=/path/to/android-ndk-r26d      # r26+ recommended for C++23 / libc++
#   cmake -B build-android -G Ninja -S . \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/android.cmake \
#         -DANDROID_ABI=x86_64                          # or arm64-v8a
#
# All of ANDROID_ABI / ANDROID_PLATFORM / ANDROID_STL may be overridden on the command line;
# the values below are only defaults.

# --- Locate the NDK ---------------------------------------------------------------------
if(NOT DEFINED ANDROID_NDK)
    if(DEFINED ENV{ANDROID_NDK})
        set(ANDROID_NDK "$ENV{ANDROID_NDK}")
    elseif(DEFINED ENV{ANDROID_NDK_HOME})
        set(ANDROID_NDK "$ENV{ANDROID_NDK_HOME}")
    elseif(DEFINED ENV{ANDROID_NDK_ROOT})
        set(ANDROID_NDK "$ENV{ANDROID_NDK_ROOT}")
    endif()
endif()

if(NOT ANDROID_NDK OR NOT EXISTS "${ANDROID_NDK}/build/cmake/android.toolchain.cmake")
    message(FATAL_ERROR
            "Android NDK not found. Set the ANDROID_NDK environment variable (or -DANDROID_NDK=...) "
            "to the root of an installed NDK (r26+ recommended for C++23/libc++).")
endif()

# --- Project defaults -------------------------------------------------------------------
# Default to x86_64 to match the x86 Android test VM; override with -DANDROID_ABI=arm64-v8a.
if(NOT DEFINED ANDROID_ABI)
    set(ANDROID_ABI "x86_64" CACHE STRING "Target Android ABI (x86_64, arm64-v8a, ...)")
endif()

# API 29 (Android 10) is the floor for AudioPlaybackCapture; MediaProjection/MediaCodec/uinput
# all work below this, so raise or lower as your target device requires.
if(NOT DEFINED ANDROID_PLATFORM)
    set(ANDROID_PLATFORM "android-29" CACHE STRING "Minimum Android API level")
endif()

# Sunshine links a large C++ surface; the shared libc++ runtime is the safe default. Ship
# libc++_shared.so alongside the binary (the NDK places it under the sysroot lib dir).
if(NOT DEFINED ANDROID_STL)
    set(ANDROID_STL "c++_shared" CACHE STRING "Android C++ runtime")
endif()

# --- Chain to the NDK toolchain ---------------------------------------------------------
include("${ANDROID_NDK}/build/cmake/android.toolchain.cmake")
