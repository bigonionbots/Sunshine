/**
 * @file src/platform/android/misc.h
 * @brief Declarations for miscellaneous Android platform helpers shared within the backend.
 */
#pragma once

// standard includes
#include <string>

// local includes
#include "src/platform/common.h"

namespace platf {
  /**
   * @brief Owning wrapper for the Android platform input backend.
   * @details Allocated by `platf::input()` and released by `platf::freeInput()`.
   *          On rooted devices this owns the `/dev/uinput` virtual device handles.
   */
  struct android_input_t {
    int uinput_mouse_fd {-1};  ///< File descriptor for the virtual mouse uinput device, or -1 if unavailable.
    int uinput_keyboard_fd {-1};  ///< File descriptor for the virtual keyboard uinput device, or -1 if unavailable.
    int uinput_touch_fd {-1};  ///< File descriptor for the virtual touchscreen uinput device, or -1 if unavailable.
    double abs_x {0};  ///< Tracked virtual cursor X, used to turn absolute moves into relative deltas.
    double abs_y {0};  ///< Tracked virtual cursor Y, used to turn absolute moves into relative deltas.
    bool use_jni_input {false};  ///< True when uinput is unavailable and JNI/Shizuku injection is used instead.
  };
}  // namespace platf
