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

## Known iteration points

These scripts are execution-tested targets, not guaranteed one-shot builds. Expect to iterate on:

1. **`libcbs` object list** (`build-ffmpeg.sh` step 4) — object names drift across FFmpeg versions.
2. **Static libcurl + pkg-config** — static curl may need `Libs.private` (OpenSSL) resolved; if
   the Sunshine link fails with missing SSL symbols, that's the cause.
3. **Option-name drift** — miniupnpc/opus/curl CMake option names change across releases; adjust
   the `-D` flags if a build ignores them.

## Remaining work after the deps build

Even once everything links and boots headless, the host does nothing useful until:
- `src/platform/android/display.cpp` gets a real capture + a software (FFmpeg) encode device, and
- `src/platform/android/input.cpp` drives `/dev/uinput` (rooted devices).

See the platform backend TODOs for the current state.
