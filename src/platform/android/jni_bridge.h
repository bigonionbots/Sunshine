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

  // --- MediaProjection screen capture (frames pushed from the app) --------------------------

  /**
   * @brief Result of copy_latest_frame().
   */
  enum class frame_status {
    ok,  ///< A frame was copied.
    none,  ///< No frame is available yet.
    resize,  ///< The capture dimensions changed; the capture pipeline should reinit.
  };

  /**
   * @brief Mark MediaProjection capture as active at the given dimensions (called from the app).
   *
   * @param width Capture width in pixels.
   * @param height Capture height in pixels.
   */
  void capture_started(int width, int height);

  /**
   * @brief Mark MediaProjection capture as stopped (called from the app).
   */
  void capture_stopped();

  /**
   * @brief Whether the app is currently delivering captured frames.
   *
   * @return True when MediaProjection capture is active.
   */
  bool capture_active();

  /**
   * @brief Current capture dimensions (valid while capture_active()).
   *
   * @param width Set to the capture width.
   * @param height Set to the capture height.
   */
  void capture_size(int &width, int &height);

  /**
   * @brief Push one RGBA_8888 frame from the app; stored (converted to BGR0) as the latest frame.
   *
   * @param rgba Pointer to the source pixel data.
   * @param width Frame width in pixels.
   * @param height Frame height in pixels.
   * @param row_stride Bytes per source row (may exceed width*4).
   */
  void push_frame(const std::uint8_t *rgba, int width, int height, int row_stride);

  /**
   * @brief Copy the latest captured frame (as AV_PIX_FMT_BGR0) into a destination buffer.
   *
   * @param dst Destination buffer.
   * @param expected_width Expected width; a mismatch yields frame_status::resize.
   * @param expected_height Expected height.
   * @param dst_row_pitch Bytes per destination row.
   * @return Copy status.
   */
  frame_status copy_latest_frame(std::uint8_t *dst, int expected_width, int expected_height, int dst_row_pitch);

  // --- AudioPlaybackCapture audio (interleaved float PCM pushed from the app) ----------------

  /**
   * @brief Mark audio capture as active with the given source format (called from the app).
   * @details The source is delivered as interleaved 32-bit float PCM. Sunshine always runs at
   *          48 kHz, so `sample_rate` is expected to be 48000; `channels` is typically 2 (the
   *          format AudioPlaybackCapture produces).
   *
   * @param sample_rate Source sample rate in hertz.
   * @param channels Number of interleaved source channels.
   */
  void audio_started(int sample_rate, int channels);

  /**
   * @brief Mark audio capture as stopped and drop any buffered samples (called from the app).
   */
  void audio_stopped();

  /**
   * @brief Whether the app is currently delivering captured audio.
   *
   * @return True when AudioPlaybackCapture is active.
   */
  bool audio_active();

  /**
   * @brief Push interleaved float PCM captured by the app into the ring buffer.
   *
   * @param samples Pointer to interleaved float samples.
   * @param count Number of float samples (frames * source channels).
   */
  void push_audio(const float *samples, int count);

  /**
   * @brief Drain exactly `frames` audio frames into `dst`, remapping to `out_channels`.
   * @details Blocks up to `timeout_ms` for enough buffered samples. Source stereo is copied to
   *          the first two output channels; extra output channels are zero-filled and mono is
   *          the average of L/R. On timeout the destination is filled with silence so the audio
   *          stream keeps flowing.
   *
   * @param dst Destination for interleaved float PCM (`frames * out_channels` samples).
   * @param frames Number of audio frames requested.
   * @param out_channels Number of output channels to produce.
   * @param timeout_ms Maximum time to wait for buffered samples.
   * @return True when real captured audio was written, false when silence was written.
   */
  bool read_audio(float *dst, int frames, int out_channels, int timeout_ms);
}  // namespace jni
