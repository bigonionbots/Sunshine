/**
 * @file src/platform/android/display.cpp
 * @brief Android screen-capture backend.
 * @details Proof-of-concept capture backed by the Android `screencap` tool (SurfaceFlinger
 *          screenshot). Each frame is grabbed by running `screencap`, parsing its raw
 *          {width,height,format[,dataspace]} header, and copying the RGBA_8888 pixels into a
 *          system-memory image (swapping R/B to the AV_PIX_FMT_BGR0 the software encoder expects).
 *          It is simple and rooted-friendly but slow (a process spawn per frame), so it is a
 *          stepping stone toward a real backend (native SurfaceComposerClient, or MediaProjection
 *          via the JNI bridge, feeding a MediaCodec encode device). On any failure it falls back
 *          to black frames so the streaming session keeps running.
 */
// standard includes
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

// local includes
#include "src/platform/android/jni_bridge.h"
#include "src/platform/common.h"
#include "src/logging.h"

using namespace std::literals;

namespace platf {

  namespace {
    constexpr int BYTES_PER_PIXEL = 4;  ///< Bytes per pixel for the 32-bpp capture buffer.
    constexpr auto SCREENCAP_PATH = "/system/bin/screencap";  ///< Path to the Android screencap tool.

    /**
     * @brief Parsed screencap raw header.
     */
    struct screencap_header_t {
      std::uint32_t width;  ///< Frame width in pixels.
      std::uint32_t height;  ///< Frame height in pixels.
      std::size_t pixel_offset;  ///< Byte offset within the raw buffer where pixel data begins.
    };

    /**
     * @brief Run the Android `screencap` tool and read its raw stdout.
     *
     * @param out Destination buffer populated with screencap's raw output.
     * @return True if any output was captured.
     */
    bool run_screencap(std::vector<std::uint8_t> &out) {
      out.clear();
      FILE *pipe = popen(SCREENCAP_PATH, "r");
      if (!pipe) {
        return false;
      }
      std::uint8_t chunk[1 << 16];
      std::size_t n;
      while ((n = fread(chunk, 1, sizeof(chunk), pipe)) > 0) {
        out.insert(out.end(), chunk, chunk + n);
      }
      pclose(pipe);
      return !out.empty();
    }

    /**
     * @brief Parse a screencap raw header.
     * @details The header is {width, height, format} as uint32, optionally followed by a
     *          dataspace uint32 on newer Android, then tightly-packed 32-bit pixels. The optional
     *          field (and thus a 12- or 16-byte header) is detected by matching the total size.
     *
     * @param raw Raw screencap output.
     * @param hdr Populated with the parsed dimensions and pixel offset on success.
     * @return True when the header is valid and the payload is 32-bpp tightly packed.
     */
    bool parse_screencap(const std::vector<std::uint8_t> &raw, screencap_header_t &hdr) {
      if (raw.size() < 12) {
        return false;
      }
      std::uint32_t w;
      std::uint32_t h;
      std::memcpy(&w, raw.data(), 4);
      std::memcpy(&h, raw.data() + 4, 4);
      if (w == 0 || h == 0) {
        return false;
      }
      const std::size_t pixels = static_cast<std::size_t>(w) * h * BYTES_PER_PIXEL;
      if (raw.size() == 12 + pixels) {
        hdr.pixel_offset = 12;
      } else if (raw.size() == 16 + pixels) {
        hdr.pixel_offset = 16;  // header includes the dataspace field
      } else {
        return false;  // unexpected pixel format (not 32-bpp) or row stride padding
      }
      hdr.width = w;
      hdr.height = h;
      return true;
    }
  }  // namespace

  /**
   * @brief System-memory image backed by a heap allocation.
   */
  struct android_img_t: img_t {
    std::vector<std::uint8_t> buffer;  ///< Backing storage for the image pixels.
  };

  /**
   * @brief Android display backend that captures the screen via the `screencap` tool.
   */
  struct android_display_t: display_t {
    std::vector<std::uint8_t> capture_buf;  ///< Reused buffer for raw screencap output.

    /**
     * @brief Capture loop that grabs screen frames until interrupted.
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

        if (!grab_screen(img.get())) {
          dummy_img(img.get());  // fall back to black on any capture failure
        }

        if (!push_captured_image_cb(std::move(img), true)) {
          return capture_e::ok;  // break requested
        }

        // Light floor; the screencap process latency dominates the effective frame rate.
        std::this_thread::sleep_for(8ms);
      }
    }

    /**
     * @brief Grab one screen frame into the image via `screencap`.
     *
     * @param img Destination image (must match the negotiated display dimensions).
     * @return True on a successful capture and conversion; false on any failure.
     */
    bool grab_screen(img_t *img) {
      if (!img || !img->data) {
        return false;
      }
      if (!run_screencap(capture_buf)) {
        return false;
      }
      screencap_header_t hdr;
      if (!parse_screencap(capture_buf, hdr)) {
        return false;
      }
      if (static_cast<int>(hdr.width) != img->width || static_cast<int>(hdr.height) != img->height) {
        // Resolution changed since session negotiation; skip rather than corrupt the frame.
        return false;
      }

      const std::uint8_t *src = capture_buf.data() + hdr.pixel_offset;
      std::uint8_t *dst = img->data;
      const std::size_t px = static_cast<std::size_t>(hdr.width) * hdr.height;
      // screencap yields RGBA_8888 (R,G,B,A); the software encoder reads AV_PIX_FMT_BGR0
      // (B,G,R,X). Swap R and B while copying. (If red/blue look swapped, drop the swap.)
      for (std::size_t i = 0; i < px; ++i) {
        dst[i * 4 + 0] = src[i * 4 + 2];  // B
        dst[i * 4 + 1] = src[i * 4 + 1];  // G
        dst[i * 4 + 2] = src[i * 4 + 0];  // R
        dst[i * 4 + 3] = 0;  // X
      }
      return true;
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
      // Software encode path: return the base device (data == nullptr). video.cpp detects this
      // and substitutes its own avcodec_software_encode_device_t, which performs the CPU
      // pixel-format conversion (sws_scale) from our system-memory frames into the x264/x265
      // encoder. This mirrors the software branch of the Linux x11grab backend. A MediaCodec-
      // backed hardware device would be returned here in the future.
      return std::make_unique<avcodec_encode_device_t>();
    }
  };

  std::shared_ptr<display_t> display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config) {
    auto metrics = jni::display_metrics();
    int w = metrics.width;
    int h = metrics.height;

    // Probe the real screen size via screencap so the encoder frame size matches the device.
    {
      std::vector<std::uint8_t> raw;
      screencap_header_t hdr;
      if (run_screencap(raw) && parse_screencap(raw, hdr)) {
        w = static_cast<int>(hdr.width);
        h = static_cast<int>(hdr.height);
        BOOST_LOG(info) << "android: screencap available, capturing "sv << w << "x"sv << h;
      } else {
        BOOST_LOG(warning) << "android: screencap probe failed; frames will be black ("sv << w << "x"sv << h << ")"sv;
      }
    }

    auto disp = std::make_shared<android_display_t>();
    disp->width = w;
    disp->height = h;
    disp->offset_x = 0;
    disp->offset_y = 0;
    disp->env_width = w;
    disp->env_height = h;
    disp->env_logical_width = w;
    disp->env_logical_height = h;
    disp->logical_width = w;
    disp->logical_height = h;
    return disp;
  }

  std::vector<std::string> display_names(mem_type_e hwdevice_type) {
    return {"android-screen"s};
  }

}  // namespace platf
