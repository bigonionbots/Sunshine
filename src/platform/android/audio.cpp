/**
 * @file src/platform/android/audio.cpp
 * @brief Android audio-capture backend.
 * @details Captures playback audio via AudioPlaybackCapture (Android 10+). The host app owns the
 *          AudioRecord and pushes interleaved float PCM across the JNI bridge; this backend drains
 *          that ring buffer into the streaming pipeline. When no app is attached (headless bring-up)
 *          the microphone yields silence, which keeps the audio stream alive without erroring.
 */
// standard includes
#include <memory>

// local includes
#include "src/platform/android/jni_bridge.h"
#include "src/platform/common.h"

namespace platf {

  namespace {
    /**
     * @brief Microphone stream backed by the app's AudioPlaybackCapture ring buffer.
     */
    class android_mic_t: public mic_t {
    public:
      /**
       * @brief Construct a microphone for the requested layout.
       *
       * @param channels Number of output channels expected by the pipeline.
       * @param frame_size Number of frames delivered per sample() call.
       */
      android_mic_t(int channels, std::uint32_t frame_size):
          channels {channels},
          frame_size {frame_size} {}

      /**
       * @brief Fill a frame with captured (or silent) interleaved float PCM.
       *
       * @param frame_buffer Destination sized to frame_size * channels.
       * @return capture_e::ok once the buffer is filled (silence counts as a valid frame).
       */
      capture_e sample(std::vector<float> &frame_buffer) override {
        // read_audio blocks briefly for real audio and falls back to silence on timeout, so the
        // stream stays real-time whether or not an app is currently playing sound.
        jni::read_audio(frame_buffer.data(), static_cast<int>(frame_size), channels, 100);
        return capture_e::ok;
      }

    private:
      int channels;  ///< Output channel count.
      std::uint32_t frame_size;  ///< Frames per sample() call.
    };

    /**
     * @brief Audio controller for Android. Sinks are not manageable, so those calls are no-ops.
     */
    class android_audio_control_t: public audio_control_t {
    public:
      /**
       * @brief No routable sinks on Android; report success without changing anything.
       *
       * @param sink Ignored.
       * @return Always 0 (success).
       */
      int set_sink(const std::string &sink) override {
        (void) sink;
        return 0;
      }

      /**
       * @brief Create a microphone for the requested layout.
       *
       * @param mapping Opus channel mapping (unused; stereo capture is remapped in read_audio).
       * @param channels Number of output channels.
       * @param sample_rate Output sample rate in hertz.
       * @param frame_size Frames per capture.
       * @param continuous Whether silence should keep being emitted (always true here).
       * @param host_audio_enabled Whether host playback stays enabled (unused).
       * @return Microphone capture object.
       */
      std::unique_ptr<mic_t> microphone(const std::uint8_t *mapping, int channels, std::uint32_t sample_rate, std::uint32_t frame_size, bool continuous, [[maybe_unused]] bool host_audio_enabled) override {
        (void) mapping;
        (void) continuous;
        (void) sample_rate;
        return std::make_unique<android_mic_t>(channels, frame_size);
      }

      /**
       * @brief Every sink is considered available (there is only the implicit device output).
       *
       * @param sink Ignored.
       * @return Always true.
       */
      bool is_sink_available(const std::string &sink) override {
        (void) sink;
        return true;
      }

      /**
       * @brief Report a placeholder sink so the audio pipeline initializes.
       * @details Android has no routable/virtual sinks, but the caller treats a missing sink_t as
       *          a hard failure and discards the whole controller. Returning a valid sink_t with a
       *          non-empty host name (and no virtual sinks) keeps audio capture alive; set_sink is
       *          a no-op, so the name itself is never acted on.
       *
       * @return A sink_t naming the implicit device output.
       */
      std::optional<sink_t> sink_info() override {
        // In screencap/privacy mode the app never calls nativeAudioStarted(), so no real audio
        // is available. Returning nullopt tells the pipeline "there will be no audio" and stops
        // it from sending silence packets that compete with video for bandwidth.
        if (!jni::audio_active()) {
          return std::nullopt;
        }
        sink_t sink;
        sink.host = "android-playback";
        return sink;
      }
    };
  }  // namespace

  std::unique_ptr<audio_control_t> audio_control() {
    return std::make_unique<android_audio_control_t>();
  }

}  // namespace platf
