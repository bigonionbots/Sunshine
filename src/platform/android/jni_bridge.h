/**
 * @file src/platform/android/jni_bridge.h
 * @brief Boundary between the native Sunshine core and the Android/Java runtime.
 * @details Screen capture (MediaProjection), hardware encode (MediaCodec), audio capture
 *          (AudioPlaybackCapture) and non-root input all live behind Java APIs. A host app or
 *          service owns those objects and shuttles data across this bridge. On a rooted device
 *          the native code can bypass much of this (uinput, direct capture), so every function
 *          here is optional and returns a safe default when no JVM is attached — which is what
 *          lets the binary boot headless during bring-up.
 */
#pragma once

// standard includes
#include <cstdint>
#include <optional>
#include <string>

namespace jni {
  /**
   * @brief Metrics describing the capturable Android display.
   */
  struct display_metrics_t {
    int width {1920};  ///< Physical width of the display in pixels.
    int height {1080};  ///< Physical height of the display in pixels.
    int density_dpi {160};  ///< Display density in dots per inch.
    double refresh_rate {60.0};  ///< Display refresh rate in hertz.
  };

  /**
   * @brief Register the Java VM and application context for later JNI calls.
   * @details Intended to be called from `JNI_OnLoad` in the host app. When never called,
   *          all other bridge functions fall back to their headless defaults.
   *
   * @param vm Pointer to the process-wide `JavaVM`.
   * @param app_context Global reference to the host `android.content.Context`.
   */
  void set_vm(void *vm, void *app_context);

  /**
   * @brief Query the user-facing device name for host advertising.
   * @return Device name when a JVM is attached, otherwise `std::nullopt`.
   */
  std::optional<std::string> device_name();

  /**
   * @brief Query the metrics of the display selected for capture.
   * @return Display metrics reported by the host app, or built-in defaults when headless.
   */
  display_metrics_t display_metrics();

  /**
   * @brief Open a URL using the host app (Intent.ACTION_VIEW).
   *
   * @param url The URL to open.
   */
  void open_url(const std::string &url);
}  // namespace jni
