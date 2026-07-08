# android specific compile definitions
# Android registers as UNIX in CMake and would otherwise fall through to linux.cmake.
# This file wires up the (currently minimal) Android platform backend instead.

add_compile_definitions(SUNSHINE_PLATFORM="android")

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

# NDK system libraries. `log` and `android` are always needed; the media/aaudio libs
# come online as the corresponding backends are implemented.
list(APPEND PLATFORM_LIBRARIES
        android
        log)

# Media/audio/capture NDK libs — uncomment as the backends land:
# list(APPEND PLATFORM_LIBRARIES mediandk camera2ndk aaudio EGL GLESv3)
