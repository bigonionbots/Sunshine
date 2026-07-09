# android specific compile definitions
# Android registers as UNIX in CMake and would otherwise fall through to linux.cmake.
# This file wires up the (currently minimal) Android platform backend instead.

add_compile_definitions(SUNSHINE_PLATFORM="android")

# Note: -fexperimental-library (needed for libc++'s <format>) is set globally in the top-level
# CMakeLists.txt so it also applies to subprojects added before this file.

# The cross-built native deps (curl/openssl/opus/miniupnpc) are discovered via pkg-config and
# find_package, which supply the library names but not the -L search path when cross-compiling.
# Add the lib dir of each user-supplied prefix so the linker can resolve -lcurl/-lssl/etc.
#
# Crucially, skip NDK-internal prefixes: the toolchain augments CMAKE_PREFIX_PATH with its own
# root, and adding its host `lib` dir would put the host build's libc++ ahead of the Android
# sysroot's on the search path — silently shadowing libc++ (both are x86_64 ELF) and leaving
# std::__ndk1 symbols undefined at link time.
foreach(_prefix ${CMAKE_PREFIX_PATH})
    if(EXISTS "${_prefix}/lib")
        if(ANDROID_NDK AND "${_prefix}" MATCHES "^${ANDROID_NDK}")
            # NDK-internal prefix; do not add its host lib dir.
        else()
            link_directories("${_prefix}/lib")
        endif()
    endif()
endforeach()

# The Android backend is intentionally small: it mirrors the self-contained macOS
# layout rather than the sprawling Linux one. Capture/encode/audio/input are brokered
# either through the NDK directly (rooted devices) or across JNI to a host app/service.
set(PLATFORM_TARGET_FILES
        "${CMAKE_SOURCE_DIR}/src/platform/android/misc.h"
        "${CMAKE_SOURCE_DIR}/src/platform/android/misc.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/android/display.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/android/audio.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/android/input.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/android/publish.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/android/jni_bridge.h"
        "${CMAKE_SOURCE_DIR}/src/platform/android/jni_bridge.cpp")

# The JNI entry point is only needed when building the shared library for the app.
if(SUNSHINE_BUILD_ANDROID_LIBRARY)
    list(APPEND PLATFORM_TARGET_FILES "${CMAKE_SOURCE_DIR}/src/platform/android/jni_main.cpp")
endif()

# NDK system libraries. `log` and `android` are always needed; the media/aaudio libs
# come online as the corresponding backends are implemented.
list(APPEND PLATFORM_LIBRARIES
        android
        log)

# Media/audio/capture NDK libs — uncomment as the backends land:
# list(APPEND PLATFORM_LIBRARIES mediandk camera2ndk aaudio EGL GLESv3)
