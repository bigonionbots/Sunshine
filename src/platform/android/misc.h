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
   * @brief Create and configure a uinput virtual mouse device.
   * @details Intended to be called from the Shizuku UserService (running as shell) which has
   *          SELinux permission to open /dev/uinput. The open fd is returned to the caller;
   *          ownership transfers to the caller (it should be wrapped in a ParcelFileDescriptor
   *          and closed after Binder transmission).
   *
   * @return Open uinput fd on success, -1 on failure.
   */
  int android_create_uinput_mouse();

  /**
   * @brief Create and configure a uinput virtual keyboard device (same context as mouse).
   *
   * @return Open uinput fd on success, -1 on failure.
   */
  int android_create_uinput_keyboard();
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
  };
}  // namespace platf
