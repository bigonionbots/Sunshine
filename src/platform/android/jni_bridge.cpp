/**
 * @file src/platform/android/jni_bridge.cpp
 * @brief Headless-safe default implementations of the Android JNI bridge.
 * @details These stubs let the native core link and run without an attached JVM. As the host
 *          app lands, `set_vm()` will capture real handles and the getters will call into Java.
 */
// standard includes
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
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

    video_callbacks_t g_video_cb;  ///< App-side MediaCodec callbacks (empty when headless).
    std::atomic<bool> g_video_active {false};  ///< Whether MediaCodec surface encode is running.

    std::mutex g_encoded_mutex;  ///< Guards the encoded access-unit queue.
    std::condition_variable g_encoded_cv;  ///< Signals newly pushed access units.

    /**
     * @brief One encoded access unit awaiting transmission.
     */
    struct encoded_au_t {
      std::vector<std::uint8_t> data;  ///< Access-unit bytes.
      bool idr {false};  ///< Whether this is a keyframe.
    };

    std::deque<encoded_au_t> g_encoded;  ///< Encoded access units from MediaCodec.

    /// Cap the encoded backlog; if the pipeline stalls, keep only the most recent frames.
    constexpr std::size_t k_encoded_max = 8;

    std::mutex g_audio_mutex;  ///< Guards the audio ring buffer and format.
    std::condition_variable g_audio_cv;  ///< Signals newly pushed audio samples.
    std::deque<float> g_audio_buf;  ///< Interleaved float PCM awaiting consumption.
    std::atomic<bool> g_audio_active {false};  ///< Whether the app is delivering audio.
    int g_audio_channels = 2;  ///< Source channel count (guarded by g_audio_mutex).

    /// Cap the backlog so a stalled consumer can't grow the buffer without bound (~0.5s stereo).
    constexpr std::size_t k_audio_buf_max = 48000 * 2 / 2;
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

  void audio_started(int sample_rate, int channels) {
    (void) sample_rate;  // Sunshine always resamples/consumes at 48 kHz.
    std::lock_guard<std::mutex> lock(g_audio_mutex);
    g_audio_channels = channels > 0 ? channels : 2;
    g_audio_buf.clear();
    g_audio_active = true;
  }

  void audio_stopped() {
    std::lock_guard<std::mutex> lock(g_audio_mutex);
    g_audio_active = false;
    g_audio_buf.clear();
    g_audio_cv.notify_all();
  }

  bool audio_active() {
    return g_audio_active.load();
  }

  void push_audio(const float *samples, int count) {
    if (!samples || count <= 0) {
      return;
    }
    std::lock_guard<std::mutex> lock(g_audio_mutex);
    if (!g_audio_active) {
      return;
    }
    // Drop the oldest samples if the consumer has fallen behind, keeping latency bounded.
    if (g_audio_buf.size() + count > k_audio_buf_max) {
      const std::size_t overflow = g_audio_buf.size() + count - k_audio_buf_max;
      g_audio_buf.erase(g_audio_buf.begin(), g_audio_buf.begin() + std::min(overflow, g_audio_buf.size()));
    }
    g_audio_buf.insert(g_audio_buf.end(), samples, samples + count);
    g_audio_cv.notify_one();
  }

  bool read_audio(float *dst, int frames, int out_channels, int timeout_ms) {
    if (!dst || frames <= 0 || out_channels <= 0) {
      return false;
    }
    std::unique_lock<std::mutex> lock(g_audio_mutex);
    const int src_channels = g_audio_channels;
    const std::size_t needed = static_cast<std::size_t>(frames) * src_channels;
    const bool have = g_audio_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] {
      return !g_audio_active || g_audio_buf.size() >= needed;
    });

    if (!have || g_audio_buf.size() < needed) {
      // Timed out or capture stopped: emit silence so the audio stream keeps flowing.
      std::memset(dst, 0, static_cast<std::size_t>(frames) * out_channels * sizeof(float));
      return false;
    }

    for (int f = 0; f < frames; ++f) {
      float l = g_audio_buf.front();
      g_audio_buf.pop_front();
      float r = l;
      for (int c = 1; c < src_channels; ++c) {
        const float v = g_audio_buf.front();
        g_audio_buf.pop_front();
        if (c == 1) {
          r = v;
        }
      }
      float *out = dst + static_cast<std::size_t>(f) * out_channels;
      if (out_channels == 1) {
        out[0] = 0.5f * (l + r);
      } else {
        out[0] = l;
        out[1] = r;
        for (int c = 2; c < out_channels; ++c) {
          out[c] = 0.0f;
        }
      }
    }
    return true;
  }

  void set_video_callbacks(video_callbacks_t callbacks) {
    g_video_cb = std::move(callbacks);
  }

  bool video_encoder_available() {
    return static_cast<bool>(g_video_cb.start);
  }

  bool video_encoder_start(int width, int height, int codec, int fps, int bitrate_bps) {
    if (!g_video_cb.start) {
      return false;
    }
    {
      std::lock_guard<std::mutex> lock(g_encoded_mutex);
      g_encoded.clear();
    }
    if (!g_video_cb.start(width, height, codec, fps, bitrate_bps)) {
      return false;
    }
    g_video_active = true;
    return true;
  }

  void video_encoder_stop() {
    g_video_active = false;
    if (g_video_cb.stop) {
      g_video_cb.stop();
    }
    std::lock_guard<std::mutex> lock(g_encoded_mutex);
    g_encoded.clear();
    g_encoded_cv.notify_all();
  }

  bool video_encoder_active() {
    return g_video_active.load();
  }

  void video_request_keyframe() {
    if (g_video_cb.request_keyframe) {
      g_video_cb.request_keyframe();
    }
  }

  void video_set_bitrate(int bitrate_bps) {
    if (g_video_cb.set_bitrate) {
      g_video_cb.set_bitrate(bitrate_bps);
    }
  }

  void push_encoded(const std::uint8_t *data, int size, int flags, std::int64_t pts_us) {
    (void) pts_us;
    if (!data || size <= 0) {
      return;
    }
    // Drop standalone codec-config (SPS/PPS/VPS) buffers: MediaCodec is configured to prepend
    // headers to each sync frame, so these are redundant.
    constexpr int flag_keyframe = 0x1;
    constexpr int flag_codec_config = 0x2;
    if (flags & flag_codec_config) {
      return;
    }
    std::lock_guard<std::mutex> lock(g_encoded_mutex);
    if (!g_video_active) {
      return;
    }
    if (g_encoded.size() >= k_encoded_max) {
      g_encoded.pop_front();  // pipeline stalled; keep the freshest frames
    }
    encoded_au_t au;
    au.data.assign(data, data + size);
    au.idr = (flags & flag_keyframe) != 0;
    g_encoded.push_back(std::move(au));
    g_encoded_cv.notify_one();
  }

  bool pull_encoded(std::vector<std::uint8_t> &out, bool &is_idr, int timeout_ms) {
    std::unique_lock<std::mutex> lock(g_encoded_mutex);
    const bool have = g_encoded_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] {
      return !g_encoded.empty() || !g_video_active;
    });
    if (!have || g_encoded.empty()) {
      return false;
    }
    out = std::move(g_encoded.front().data);
    is_idr = g_encoded.front().idr;
    g_encoded.pop_front();
    return true;
  }

  bool video_output_wait(int timeout_ms) {
    std::unique_lock<std::mutex> lock(g_encoded_mutex);
    return g_encoded_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] {
      return !g_encoded.empty() || !g_video_active;
    }) && !g_encoded.empty();
  }

}  // namespace jni
