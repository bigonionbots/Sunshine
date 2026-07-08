# Reproducible build host for cross-compiling Sunshine's FFmpeg dependency for Android.
#
# This image pins the Android NDK and the host tooling (git, cmake, ninja, nasm, perl, ...) so
# the dependency builds are reproducible regardless of the developer's machine. It builds the
# native dependencies only (see build-deps.sh / build-ffmpeg.sh); the artifacts are written to
# a mounted volume and then consumed by a normal Sunshine CMake configure.
#
# Build the image:
#   docker build -t sunshine-android-deps scripts/android
#
# Run it (mount the repo so artifacts land under it, and the scripts read-only):
#   docker run --rm -e ABI=x86_64 -e API=29 \
#     -v "$PWD:/src" -w /src \
#     -v "$PWD/scripts/android:/scripts:ro" \
#     sunshine-android-deps \
#     bash /scripts/build-all.sh
#
# build-all.sh runs both dependency scripts and prints the exact Sunshine configure command.
# See scripts/android/README.md for the full flow.

FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive

# NDK version — r26+ is recommended for C++23 / libc++. Override with --build-arg NDK_VERSION=...
ARG NDK_VERSION=r26d

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        cmake \
        curl \
        git \
        make \
        nasm \
        ninja-build \
        perl \
        pkg-config \
        python3 \
        unzip \
        yasm \
    && rm -rf /var/lib/apt/lists/*

# Install the Android NDK.
RUN curl -fsSL "https://dl.google.com/android/repository/android-ndk-${NDK_VERSION}-linux.zip" \
        -o /tmp/ndk.zip \
    && unzip -q /tmp/ndk.zip -d /opt \
    && rm /tmp/ndk.zip

ENV ANDROID_NDK=/opt/android-ndk-${NDK_VERSION}

WORKDIR /work

# The build script is mounted at runtime (-v .../scripts/android:/scripts:ro) so it can be
# iterated on without rebuilding the image. Default command prints usage.
CMD ["bash", "-lc", "echo 'Mount scripts and run: bash /scripts/build-ffmpeg.sh (see Dockerfile header)'"]
