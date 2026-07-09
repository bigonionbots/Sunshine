/**
 * @file src/platform/android/jni_bridge.cpp
 * @brief Headless-safe default implementations of the Android JNI bridge.
 * @details These stubs let the native core link and run without an attached JVM. As the host
 *          app lands, `set_vm()` will capture real handles and the getters will call into Java.
 */
// standard includes
#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

// local includes
#include "src/platform/android/jni_bridge.h"

namespace jni {

  namespace {
    void *g_vm = nullptr;  ///< Cached JavaVM pointer, or null when running headless.
    void *g_app_context = nullptr;  ///< Cached global reference to the host Context.

    std::mutex g_frame_mutex;  ///< Guards the latest captured frame.
    std::vector<std::uint8_t> g_frame;  ///< Latest frame, tightly packed BGR0 (width*height*4).
    int g_frame_w = 0;  ///< Latest frame width.
    int g_frame_h = 0;  ///< Latest frame height.

    std::atomic<bool> g_capture_active {false};  ///< Whether the app is delivering frames.
    std::atomic<int> g_capture_w {0};  ///< Advertised capture width.
    std::atomic<int> g_capture_h {0};  ///< Advertised capture height.
  }  // namespace

  void set_vm(void *vm, void *app_context) {
    g_vm = vm;
    g_app_context = app_context;
  }

  std::optional<std::string> device_name() {
    // TODO: when g_vm is set, read android.os.Build.MODEL via JNI.
    return std::nullopt;
  }

  display_metrics_t display_metrics() {
    // TODO: when g_vm is set, read the real DisplayMetrics from the host app.
    return {};
  }

  void open_url(const std::string &url) {
    // TODO: when g_vm is set, dispatch an Intent.ACTION_VIEW from the host app.
    (void) url;
  }

  void capture_started(int width, int height) {
    g_capture_w = width;
    g_capture_h = height;
    g_capture_active = true;
  }

  void capture_stopped() {
    g_capture_active = false;
    std::lock_guard<std::mutex> lock(g_frame_mutex);
    g_frame.clear();
    g_frame_w = 0;
    g_frame_h = 0;
  }

  bool capture_active() {
    return g_capture_active.load();
  }

  void capture_size(int &width, int &height) {
    width = g_capture_w.load();
    height = g_capture_h.load();
  }

  void push_frame(const std::uint8_t *rgba, int width, int height, int row_stride) {
    if (!rgba || width <= 0 || height <= 0) {
      return;
    }
    std::lock_guard<std::mutex> lock(g_frame_mutex);
    g_frame.resize(static_cast<std::size_t>(width) * height * 4);
    // Convert RGBA_8888 (possibly strided) to tightly-packed AV_PIX_FMT_BGR0 (B,G,R,X).
    for (int y = 0; y < height; ++y) {
      const std::uint8_t *s = rgba + static_cast<std::size_t>(y) * row_stride;
      std::uint8_t *d = g_frame.data() + static_cast<std::size_t>(y) * width * 4;
      for (int x = 0; x < width; ++x) {
        d[0] = s[2];  // B
        d[1] = s[1];  // G
        d[2] = s[0];  // R
        d[3] = 0;  // X
        s += 4;
        d += 4;
      }
    }
    g_frame_w = width;
    g_frame_h = height;
  }

  frame_status copy_latest_frame(std::uint8_t *dst, int expected_width, int expected_height, int dst_row_pitch) {
    std::lock_guard<std::mutex> lock(g_frame_mutex);
    if (g_frame.empty()) {
      return frame_status::none;
    }
    if (g_frame_w != expected_width || g_frame_h != expected_height) {
      return frame_status::resize;
    }
    const std::size_t src_pitch = static_cast<std::size_t>(g_frame_w) * 4;
    if (static_cast<std::size_t>(dst_row_pitch) == src_pitch) {
      std::memcpy(dst, g_frame.data(), src_pitch * g_frame_h);
    } else {
      for (int y = 0; y < g_frame_h; ++y) {
        std::memcpy(dst + static_cast<std::size_t>(y) * dst_row_pitch,
                    g_frame.data() + static_cast<std::size_t>(y) * src_pitch,
                    src_pitch);
      }
    }
    return frame_status::ok;
  }

}  // namespace jni
