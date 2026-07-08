/**
 * @file src/platform/android/audio.cpp
 * @brief Android audio-capture backend.
 * @details Not yet implemented. Returning a null controller starts the host silent, which is a
 *          valid state for the streaming pipeline. The real backend will capture playback audio
 *          via AudioPlaybackCapture (Android 10+) or AAudio and expose it as a `mic_t`.
 */
// local includes
#include "src/platform/common.h"

namespace platf {

  std::unique_ptr<audio_control_t> audio_control() {
    // TODO: implement AudioPlaybackCapture/AAudio capture. Null == host streams no audio.
    return nullptr;
  }

}  // namespace platf
