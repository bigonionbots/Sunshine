# android specific dependencies
#
# The Android backend deliberately avoids the desktop capture/encode dependency stack
# (VAAPI, CUDA, libdrm, libevdev, PulseAudio, Wayland, X11). The shared UNIX-level
# dependencies resolved in dependencies/common.cmake still apply:
#   - OpenSSL, Boost, Opus, nlohmann_json  -> must be provided as NDK/Bionic builds
#   - moonlight-common-c / enet            -> submodule, builds from source
#   - Simple-Web-Server                    -> header-only, builds from source
#   - libdisplaydevice                     -> reuses its generic (non-Windows/macOS) path
#   - miniupnpc, libcurl                   -> must be provided as NDK/Bionic builds
#   - FFmpeg                               -> dependencies/ffmpeg.cmake ships desktop prebuilts
#                                             only; an Android build must be supplied instead.
#
# The NDK sysroot provides EGL/GLESv3/mediandk/aaudio/camera2ndk directly; those are linked
# from cmake/compile_definitions/android.cmake as the corresponding backends are implemented.
#
# Nothing to resolve here yet — this file exists so the platform dispatch in
# dependencies/common.cmake has an Android branch and does not fall through to linux.cmake.
