/**
 * @file src/platform/android/display.cpp
 * @brief Android screen-capture backend.
 * @details The bring-up implementation is a software backend that hands the streaming pipeline
 *          black frames sized to the display, so the RTSP/ENet session can negotiate and run
 *          end-to-end before real capture exists. The real backend will source frames from
 *          MediaProjection via AImageReader/AHardwareBuffer (or direct DRM on rooted devices)
 *          and expose a MediaCodec-backed encode device, analogous to how the macOS backend
 *          wraps ScreenCaptureKit + VideoToolbox.
 */
// standard includes
#include <chrono>
#include <cstring>
#include <thread>

// local includes
#include "src/platform/android/jni_bridge.h"
#include "src/platform/common.h"
#include "src/logging.h"

using namespace std::literals;

namespace platf {

  namespace {
    constexpr int BYTES_PER_PIXEL = 4;  ///< Bytes per pixel for the placeholder BGRA capture buffer.
  }  // namespace

  /**
   * @brief System-memory image backed by a heap allocation.
   */
  struct android_img_t: img_t {
    std::vector<std::uint8_t> buffer;  ///< Backing storage for the image pixels.
  };

  /**
   * @brief Software Android display backend that emits placeholder frames.
   */
  struct android_display_t: display_t {
    /**
     * @brief Capture loop that pushes placeholder frames until interrupted.
     *
     * @param push_captured_image_cb Callback invoked with each captured image.
     * @param pull_free_image_cb Callback used to obtain a free image from the pool.
     * @param cursor Unused; whether the cursor should be captured.
     * @return Capture status when the loop exits.
     */
    capture_e capture(const push_captured_image_cb_t &push_captured_image_cb, const pull_free_image_cb_t &pull_free_image_cb, bool *cursor) override {
      while (true) {
        std::shared_ptr<img_t> img;
        if (!pull_free_image_cb(img)) {
          return capture_e::ok;  // capture interrupted
        }

        dummy_img(img.get());

        if (!push_captured_image_cb(std::move(img), true)) {
          return capture_e::ok;  // break requested
        }

        // Placeholder ~60 fps pacing; real capture will be driven by frame-available callbacks.
        std::this_thread::sleep_for(16ms);
      }
    }

    std::shared_ptr<img_t> alloc_img() override {
      auto img = std::make_shared<android_img_t>();
      img->width = width;
      img->height = height;
      img->pixel_pitch = BYTES_PER_PIXEL;
      img->row_pitch = width * BYTES_PER_PIXEL;
      img->buffer.assign(static_cast<size_t>(img->row_pitch) * height, 0);
      img->data = img->buffer.data();
      return img;
    }

    int dummy_img(img_t *img) override {
      if (!img || !img->data) {
        return -1;
      }
      std::memset(img->data, 0, static_cast<size_t>(img->row_pitch) * img->height);
      return 0;
    }

    std::unique_ptr<avcodec_encode_device_t> make_avcodec_encode_device(pix_fmt_e pix_fmt) override {
      // TODO: first milestone — wire an FFmpeg software (x264/x265) encode device here so the
      // placeholder frames actually encode and stream. MediaCodec is the performant follow-up.
      BOOST_LOG(warning) << "android: make_avcodec_encode_device not yet implemented"sv;
      return nullptr;
    }
  };

  std::shared_ptr<display_t> display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config) {
    auto metrics = jni::display_metrics();

    auto disp = std::make_shared<android_display_t>();
    disp->width = metrics.width;
    disp->height = metrics.height;
    disp->offset_x = 0;
    disp->offset_y = 0;
    disp->env_width = metrics.width;
    disp->env_height = metrics.height;
    disp->env_logical_width = metrics.width;
    disp->env_logical_height = metrics.height;
    disp->logical_width = metrics.width;
    disp->logical_height = metrics.height;
    return disp;
  }

  std::vector<std::string> display_names(mem_type_e hwdevice_type) {
    return {"android-screen"s};
  }

}  // namespace platf
