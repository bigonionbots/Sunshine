# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

Sunshine is a self-hosted, low-latency game stream host for Moonlight clients. It captures the
screen and audio, encodes with hardware (or software) encoders, and streams to Moonlight clients
over a custom RTSP/ENet protocol. It runs on Linux, Windows, macOS, and (experimentally) FreeBSD.
The core is C++23; a Vue 3 + Vite web UI provides configuration and client pairing; Python (via uv)
drives tooling like localization and clang-format.

## Build & Test

The project builds with CMake + Ninja. Build type defaults to `Release`; `BUILD_TESTS` and
`BUILD_DOCS` default `ON`.

```bash
cmake -B build -G Ninja -S .   # configure (submodules must be checked out: git submodule update --init --recursive)
ninja -C build                 # build everything, including the test executable and web UI
```

- **Test executable:** `./build/tests/test_sunshine` (Google Test). Built as part of the normal build.
  - Run a single test / filter: `./build/tests/test_sunshine --gtest_filter=SuiteName.TestName`
  - List all options: `./build/tests/test_sunshine --help`
  - Test sources live in `tests/` (`tests/unit/`, `tests/integration/`, `tests/fixtures/`). The test
    build compiles all `src/` sources except `main.cpp` and links them into `test_sunshine`.
- **Web UI only:** `ninja -C build web-ui` (CMake target), or `npm run dev` (standalone Vite watch build).
- **Lint (C/C++):** formatting is enforced against `.clang-format`. Apply it in place with:
  ```bash
  uv sync --locked
  uv run --locked --no-sync lb-update-clang-format
  ```
- **Windows:** builds use MSYS2/UCRT64 (or CLANGARM64 for ARM64). Prefix commands with
  `C:\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c`. Prefix build dirs with `cmake-build-`.

Full dependency lists per platform are in `docs/building.md` and `scripts/linux_build.sh`.

## Architecture

Sunshine is a single native binary plus a set of static web assets. The C++ code in `src/` is
roughly layered as:

- **Entry & lifecycle** — `main.cpp` dispatches CLI subcommands (`creds`, `help`, `version`, …) and
  otherwise boots the service; `entry_handler.cpp`, `globals.*`, `logging.*`.
- **HTTP servers (two distinct ones):**
  - `nvhttp.cpp` — the *GameStream* HTTP(S) API that Moonlight clients talk to: server info, app
    list, client **pairing** (PIN + certificate exchange, see `crypto.*`), and launch/resume/cancel.
  - `confighttp.cpp` — the **web UI backend**: serves the built web assets and the config/admin REST
    API consumed by the Vue frontend. Backed by `config.cpp` (all settings) and `file_handler.*`.
  - `httpcommon.*` provides shared HTTP setup; `network.*` and `upnp.*` handle networking/port mapping.
- **Streaming pipeline** — `rtsp.cpp` negotiates the session; `stream.cpp` is the core RTP/ENet
  streaming engine (video/audio/control/mic channels, FEC, encryption). `stat_trackers.*` tracks
  streaming stats.
- **Capture + encode** — `video.cpp`/`video.h` and `audio.cpp` drive capture and encoder selection;
  `cbs.*` (coded bitstream) and `video_colorspace.*` handle bitstream/colorspace details.
  `src/nvenc/` is the NVIDIA NVENC encoder implementation.
- **Input injection** — `input.cpp` translates client input events into synthetic OS input.
- **App launching** — `process.cpp` launches and supervises the configured applications/games.
- **Display management** — `display_device.cpp` handles resolution/HDR/display config changes.
- **System tray** — `system_tray.*` (gated by `SUNSHINE_ENABLE_TRAY`).

### Platform abstraction

`src/platform/common.h` declares the platform-agnostic interfaces (display capture, audio capture,
input, etc.); concrete implementations live under `src/platform/{linux,windows,macos}`. On Linux
there are multiple interchangeable capture backends selected at runtime/build time — KMS/DRM
(`kmsgrab`), X11 (`x11grab`), wlroots (`wlgrab`), XDG portal (`portalgrab`), KWin (`kwingrab`),
plus VAAPI/CUDA/Vulkan encode paths. Which are compiled is controlled by `SUNSHINE_ENABLE_*` CMake
options (see `cmake/prep/options.cmake`). Linux input emulation uses inputtino
(`src/platform/linux/input/`).

### CMake layout

`CMakeLists.txt` is thin; the real logic is modularized under `cmake/` and included in order:
`prep/` (options, version, constants), `dependencies/`, `compile_definitions/`, `macros/`,
`targets/`, `packaging/`. Each has a `common.cmake` plus per-platform files
(`linux.cmake`, `windows.cmake`, `macos.cmake`, `unix.cmake`). Add platform-specific sources,
deps, and flags in the matching file rather than the top-level `CMakeLists.txt`.

### Web UI

Vue 3 + Bootstrap 5, built by Vite (`vite.config.js`). Source in `src_assets/common/assets/web/`;
each top-level page is an `.html` entry point (`index`, `config`, `apps`, `pin`, `welcome`, …)
assembled via EJS templates (`template_header.html`). Config UI tabs are Vue components under
`configs/tabs/`. Localization uses Vue I18n. The build output goes to `build/assets/web` and is
served by `confighttp.cpp`.

## Conventions (from AGENTS.md)

- **Doxygen is mandatory** — everything must be documented or the build fails. Use `/** @brief ...
  @param ... @return ... */` for primary comments and `///< ...` for inline comments. Always add or
  update Doxygen when changing code.
- **Follow `.clang-format`** for all C/C++.
- **Tests:** add/update tests for new or modified methods; target 100% coverage on changed code.
  Google Test is the framework. Coverage is measured with gcovr/Codecov; code that can't run in CI
  (e.g. needs a GPU) may use gcovr exclusion markers but should still have tests.
- **Localization:** only ever edit the English source
  `src_assets/common/assets/web/public/assets/locale/en.json` (keys sorted alphabetically) or C++
  strings wrapped in `boost::locale::translate`. Never edit other language files or commit
  extracted/compiled localization files — CrowdIn handles those.
- **Do not create issues or PRs in the LizardByte org.** If asked to open one, target a fork instead.
