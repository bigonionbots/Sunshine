/**
 * @file src/platform/android/display.cpp
 * @brief Android screen-capture backend.
 * @details Prefers direct Linux framebuffer capture (`/dev/graphics/fb0`): the framebuffer is
 *          mmap'd once and each frame is a cheap memory copy (converting to the AV_PIX_FMT_BGR0
 *          the software encoder expects, with R/B order detected at runtime from the fb bitfields).
 *          This is fast and persistent — no per-frame process spawn — and works on software-
 *          rendered android-x86 VMs where fb0 holds the live screen. On devices where fb0 is
 *          unavailable or blank (e.g. hardware-composited phones), it falls back to the `screencap`
 *          tool. Any failure emits black so the streaming session keeps running. The performant
 *          successor remains MediaProjection + a MediaCodec encode device via the JNI bridge.
 */
// standard includes
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

// platform includes
#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

// local includes
#include "src/platform/android/jni_bridge.h"
#include "src/platform/common.h"
#include "src/logging.h"
#include "src/video.h"

using namespace std::literals;

namespace platf {

  namespace {
    constexpr int BYTES_PER_PIXEL = 4;  ///< Bytes per pixel for the 32-bpp capture buffer.
    constexpr auto SCREENCAP_PATH = "/system/bin/screencap";  ///< Path to the Android screencap tool.
    constexpr auto FRAMEBUFFER_PATH = "/dev/graphics/fb0";  ///< Path to the Linux framebuffer device.

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
     *          dataspace uint32 on newer Android, then tightly-packed 32-bit pixels. Both 12- and
     *          16-byte headers are handled by matching the total size.
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
   * @brief Android display backend: framebuffer capture with a screencap fallback.
   */
  struct android_display_t: display_t {
    // Framebuffer capture state.
    int fb_fd {-1};  ///< File descriptor for the framebuffer device, or -1 if unused.
    std::uint8_t *fb_mem {nullptr};  ///< mmap'd framebuffer memory, or nullptr.
    std::size_t fb_size {0};  ///< Size of the mmap'd framebuffer region.
    int fb_stride {0};  ///< Framebuffer row length in bytes.
    bool fb_swap_rb {false};  ///< Whether the framebuffer stores red in the low byte (RGB -> needs swap).
    bool use_framebuffer {false};  ///< True when framebuffer capture is active.

    // screencap fallback state.
    std::vector<std::uint8_t> capture_buf;  ///< Reused buffer for raw screencap output.

    std::chrono::nanoseconds delay {std::chrono::milliseconds(16)};  ///< Per-frame interval from the client framerate.
    bool pending_reinit {false};  ///< Set when the display resolution changed (e.g. rotation) and the pipeline must reinit.

    ~android_display_t() override {
      if (fb_mem && fb_mem != MAP_FAILED) {
        munmap(fb_mem, fb_size);
      }
      if (fb_fd >= 0) {
        close(fb_fd);
      }
    }

    /**
     * @brief Try to open and mmap the Linux framebuffer for capture.
     *
     * @return True when the framebuffer is available as a 32-bpp source.
     */
    bool init_framebuffer() {
      fb_fd = open(FRAMEBUFFER_PATH, O_RDONLY);
      if (fb_fd < 0) {
        return false;
      }
      fb_var_screeninfo var {};
      fb_fix_screeninfo fix {};
      if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &var) < 0 || ioctl(fb_fd, FBIOGET_FSCREENINFO, &fix) < 0 || var.bits_per_pixel != 32) {
        close(fb_fd);
        fb_fd = -1;
        return false;
      }
      fb_stride = static_cast<int>(fix.line_length);
      fb_size = fix.smem_len ? fix.smem_len : static_cast<std::size_t>(fb_stride) * var.yres_virtual;
      fb_mem = static_cast<std::uint8_t *>(mmap(nullptr, fb_size, PROT_READ, MAP_SHARED, fb_fd, 0));
      if (fb_mem == MAP_FAILED) {
        fb_mem = nullptr;
        close(fb_fd);
        fb_fd = -1;
        return false;
      }
      width = static_cast<int>(var.xres);
      height = static_cast<int>(var.yres);
      // BGR0 wants blue in the low byte; swap when red is lower (RGBA/RGBX framebuffers).
      fb_swap_rb = var.red.offset < var.blue.offset;
      use_framebuffer = true;
      BOOST_LOG(info) << "android: framebuffer capture "sv << width << "x"sv << height
                      << " stride="sv << fb_stride << " swap_rb="sv << fb_swap_rb;
      return true;
    }

    /**
     * @brief Copy the current framebuffer contents into the image (converted to BGR0).
     *
     * @param img Destination image (must match the display dimensions).
     * @return True on success.
     */
    bool grab_framebuffer(img_t *img) {
      if (fb_fd < 0 || !fb_mem || !img || !img->data) {
        return false;
      }
      // Follow the currently-visible buffer for panned/double-buffered framebuffers.
      std::size_t src_off = 0;
      fb_var_screeninfo var {};
      if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &var) == 0) {
        if (static_cast<int>(var.xres) != width || static_cast<int>(var.yres) != height) {
          pending_reinit = true;  // the screen was rotated/resized; reinit at the new dimensions
          return false;
        }
        src_off = static_cast<std::size_t>(var.yoffset) * fb_stride;
      }
      if (src_off + static_cast<std::size_t>(fb_stride) * height > fb_size) {
        src_off = 0;  // guard against an out-of-range offset
      }

      const std::uint8_t *src = fb_mem + src_off;
      std::uint8_t *dst = img->data;

      // Read the framebuffer with a single bulk memcpy (sequential burst reads) rather than
      // scattered byte accesses, then apply any R/B swap on the now-cached copy. Note: on an
      // emulated/software-rendered display the readback itself is bandwidth-bound (~tens of ms
      // for a full frame), which caps the achievable capture rate regardless of access pattern.
      if (fb_stride == img->row_pitch) {
        std::memcpy(dst, src, static_cast<std::size_t>(img->row_pitch) * height);
      } else {
        const std::size_t row_bytes = static_cast<std::size_t>(width) * BYTES_PER_PIXEL;
        for (int y = 0; y < height; ++y) {
          std::memcpy(dst + static_cast<std::size_t>(y) * img->row_pitch,
                      src + static_cast<std::size_t>(y) * fb_stride,
                      row_bytes);
        }
      }

      if (fb_swap_rb) {
        // Swap R and B in place in cached memory (fast). BGR0 ignores the 4th byte.
        const std::size_t px = static_cast<std::size_t>(width) * height;
        for (std::size_t i = 0; i < px; ++i) {
          std::uint8_t *p = dst + i * 4;
          std::swap(p[0], p[2]);
        }
      }
      return true;
    }

    /**
     * @brief Grab one screen frame via the `screencap` tool (fallback path).
     *
     * @param img Destination image (must match the negotiated display dimensions).
     * @return True on a successful capture and conversion.
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
        pending_reinit = true;  // the screen was rotated/resized; reinit at the new dimensions
        return false;
      }
      const std::uint8_t *src = capture_buf.data() + hdr.pixel_offset;
      std::uint8_t *dst = img->data;
      const std::size_t px = static_cast<std::size_t>(hdr.width) * hdr.height;
      // screencap yields RGBA_8888; convert to AV_PIX_FMT_BGR0 (swap R/B).
      for (std::size_t i = 0; i < px; ++i) {
        dst[i * 4 + 0] = src[i * 4 + 2];  // B
        dst[i * 4 + 1] = src[i * 4 + 1];  // G
        dst[i * 4 + 2] = src[i * 4 + 0];  // R
        dst[i * 4 + 3] = 0;  // X
      }
      return true;
    }

    capture_e capture(const push_captured_image_cb_t &push_captured_image_cb, const pull_free_image_cb_t &pull_free_image_cb, bool *cursor) override {
      // Pace to the client-requested framerate, phase-locked to a steady clock (mirrors the Linux
      // backends). A fixed accumulator avoids the aliasing that an independent per-frame sleep
      // causes against the encoder's own pacing.
      auto next_frame = std::chrono::steady_clock::now();
      sleep_overshoot_logger.reset();

      while (true) {
        auto now = std::chrono::steady_clock::now();
        if (next_frame > now) {
          std::this_thread::sleep_for(next_frame - now);
          sleep_overshoot_logger.first_point(next_frame);
          sleep_overshoot_logger.second_point_now_and_log();
        }
        next_frame += delay;
        if (next_frame < now) {  // fell behind; resync to avoid a burst of catch-up frames
          next_frame = now + delay;
        }

        std::shared_ptr<img_t> img;
        if (!pull_free_image_cb(img)) {
          return capture_e::ok;  // capture interrupted
        }

        bool ok = use_framebuffer ? grab_framebuffer(img.get()) : grab_screen(img.get());
        if (pending_reinit) {
          return capture_e::reinit;  // display resolution changed (e.g. rotation); rebuild the pipeline
        }
        if (!ok) {
          dummy_img(img.get());  // fall back to black on any capture failure
        }

        if (!push_captured_image_cb(std::move(img), true)) {
          return capture_e::ok;  // break requested
        }
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
      // Software encode path: return the base device (data == nullptr). video.cpp detects this
      // and substitutes its own avcodec_software_encode_device_t, which performs the CPU
      // pixel-format conversion (sws_scale) from our system-memory frames into the x264/x265
      // encoder. A MediaCodec-backed hardware device would be returned here in the future.
      return std::make_unique<avcodec_encode_device_t>();
    }
  };

  std::shared_ptr<display_t> display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config) {
    auto disp = std::make_shared<android_display_t>();
    disp->delay = video::capture_frame_interval(config);

    // Prefer the direct framebuffer (fast, persistent). Fall back to screencap.
    if (!disp->init_framebuffer()) {
      auto metrics = jni::display_metrics();
      int w = metrics.width;
      int h = metrics.height;
      std::vector<std::uint8_t> raw;
      screencap_header_t hdr;
      if (run_screencap(raw) && parse_screencap(raw, hdr)) {
        w = static_cast<int>(hdr.width);
        h = static_cast<int>(hdr.height);
        BOOST_LOG(info) << "android: framebuffer unavailable, using screencap capture "sv << w << "x"sv << h;
      } else {
        BOOST_LOG(warning) << "android: no framebuffer or screencap; frames will be black ("sv << w << "x"sv << h << ")"sv;
      }
      disp->width = w;
      disp->height = h;
    }

    disp->offset_x = 0;
    disp->offset_y = 0;
    disp->env_width = disp->width;
    disp->env_height = disp->height;
    disp->env_logical_width = disp->width;
    disp->env_logical_height = disp->height;
    disp->logical_width = disp->width;
    disp->logical_height = disp->height;
    return disp;
  }

  std::vector<std::string> display_names(mem_type_e hwdevice_type) {
    return {"android-screen"s};
  }

}  // namespace platf
