# Android (experimental) dependency builds

This directory cross-builds the native dependencies required to build the **Sunshine host**
for Android, and documents the CMake invocation that consumes them. It is part of the
exploratory Android port and is **not** wired into CI.

> [!WARNING]
> The Android host backend is a skeleton (`src/platform/android/`). These scripts get the tree
> to *link and boot*; screen capture, audio, input injection, and hardware encode are still
> stubbed or TODO. This is bring-up scaffolding, not a working streaming host.

## What builds where

| Dependency | How it is provided |
|---|---|
| OpenSSL, Opus, miniupnpc, libcurl | `build-deps.sh` → `android-deps/<abi>/` |
| FFmpeg, x264, x265 (+ `libcbs.a`) | `build-ffmpeg.sh` → `ffmpeg-android/<abi>/ffmpeg/` |
| Boost | built automatically by Sunshine's CMake (FetchContent) using the NDK toolchain |
| moonlight-common-c/enet, Simple-Web-Server, nlohmann_json, libdisplaydevice | built as CMake subprojects during the Sunshine build |

## Prerequisites

- Android NDK **r26+** (for C++23 / libc++). Point `ANDROID_NDK` at it.
- Host tools: `git cmake ninja make perl pkg-config nasm yasm`.
- Or use the pinned `Dockerfile` in this directory, which installs all of the above.

## Quick start

```bash
export ANDROID_NDK=/path/to/android-ndk-r26d
ABI=x86_64 API=29 ./scripts/android/build-all.sh   # or ABI=arm64-v8a
```

`build-all.sh` runs both dependency scripts and prints the exact `cmake` configure command,
with all the `-D` paths filled in, for the main Sunshine build.

### Reproducible builds via Docker

```bash
docker build -t sunshine-android-deps scripts/android
docker run --rm -e ABI=x86_64 \
  -v "$PWD:/src" -w /src \
  -v "$PWD/scripts/android:/scripts:ro" \
  sunshine-android-deps bash /scripts/build-all.sh
```

## Why the FFmpeg build is special (`libcbs`)

`src/cbs.cpp` includes FFmpeg **internal** headers (`<libavcodec/cbs_h264.h>`,
`<libavcodec/cbs_h265.h>`) that a normal `make install` does not ship, and whose symbols are
not exported from stock `libavcodec`. `build-ffmpeg.sh` therefore archives the CBS object files
into a separate `libcbs.a` and copies the internal headers. **This is the step most likely to
need a tweak** between FFmpeg versions — the script errors with guidance if the object names
don't match the pinned tag.

## Deploying to a device (rooted)

The build produces a native executable, not an APK. On a rooted device you can push it and run
it directly. The binary is built with `ANDROID_STL=c++_shared`, so `libc++_shared.so` must travel
with it, and `CMAKE_INSTALL_PREFIX` sets where it expects config/assets (see `build-all.sh`).

```bash
NDK=/path/to/android-ndk-r26d
ABI=x86_64                       # match your build
DEVDIR=/data/local/tmp/sunshine  # = CMAKE_INSTALL_PREFIX
LIBCXX=$NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/$ABI-linux-android/libc++_shared.so

adb shell "mkdir -p $DEVDIR/assets"
adb push build-android/sunshine "$LIBCXX" "$DEVDIR/"      # binary + C++ runtime
adb push build-android/assets/web "$DEVDIR/assets/"       # Web UI (served from assets/web)
adb push src_assets/linux/assets/apps.json "$DEVDIR/apps.json"
adb push src_assets/common/assets/*.png "$DEVDIR/assets/" # app box-art (optional, silences warnings)

adb shell su -c "cd $DEVDIR && LD_LIBRARY_PATH=. ./sunshine"
```

Reach the Web UI at `https://<device-ip>:47990` (self-signed cert). Sunshine's CSRF protection
only allowlists localhost by default, so to use the device's LAN IP add the origin to the config:

```bash
adb shell su -c 'echo "csrf_allowed_origins = https://<device-ip>:47990" >> /data/local/tmp/sunshine/sunshine.conf'
```

(Or tunnel instead: `adb forward tcp:47990 tcp:47990` and browse `https://localhost:47990`, which
is allowlisted by default.) Moonlight pairing uses a separate PIN-based path (nvhttp) and is not
affected by the CSRF setting.

## Known iteration points

These scripts are execution-tested targets, not guaranteed one-shot builds. Expect to iterate on:

1. **`libcbs` object list** (`build-ffmpeg.sh` step 4) — object names drift across FFmpeg versions.
2. **Static libcurl + pkg-config** — static curl may need `Libs.private` (OpenSSL) resolved; if
   the Sunshine link fails with missing SSL symbols, that's the cause.
3. **Option-name drift** — miniupnpc/opus/curl CMake option names change across releases; adjust
   the `-D` flags if a build ignores them.

## Current state

Verified on a rooted Android 9 x86_64 device: the host **boots, serves its full Web UI, pairs
Moonlight, streams the live screen as software-encoded H.264, and is controllable via mouse and
keyboard** end to end.

Working:
- **Software H.264 encode** — via the base `avcodec_encode_device_t` (video.cpp substitutes its
  `avcodec_software_encode_device_t`).
- **Screen capture** — `src/platform/android/display.cpp` grabs frames via the `screencap` tool.
  PoC-grade: a process spawn per frame limits the frame rate, and colorspace/range is not yet
  signaled. A native SurfaceComposerClient or MediaProjection + MediaCodec backend is the
  performant successor.
- **Mouse + keyboard input** — `src/platform/android/input.cpp` creates virtual `/dev/uinput`
  devices (needs root + permissive SELinux for `/dev/uinput`). Absolute mouse is approximate on
  the relative device; prefer Moonlight's relative mouse mode.

Still stubbed:
- **Audio** — `src/platform/android/audio.cpp` returns no controller; the stream has no audio.
- **Touch, pen, gamepad** — `input.cpp` handlers are no-ops.
- **Unicode/IME text entry** — only mapped keycodes are injected.
