/**
 * @file src/platform/android/jni_main.cpp
 * @brief JNI entry point for hosting the Sunshine core inside the Android app.
 * @details The app loads libsunshine.so and calls nativeStart() on a background thread. This
 *          points Sunshine's data directory at the app's private storage and then runs the same
 *          main() the standalone binary uses, which boots the servers and blocks until shutdown.
 */
// standard includes
#include <csignal>
#include <cstdint>
#include <cstdlib>

// platform includes
#include <jni.h>
#include <unistd.h>

// local includes
#include "src/platform/android/jni_bridge.h"

/**
 * @brief Sunshine's entry point, compiled into the shared library.
 */
extern "C" int main(int argc, char *argv[]);

namespace {
  JavaVM *g_jvm = nullptr;  ///< Cached Java VM for upcalls from native threads.
  jobject g_video_bridge = nullptr;  ///< Global ref to the app's MediaCodec bridge object.
  jmethodID g_mid_start = nullptr;  ///< VideoBridge.startEncoder(IIIII)Z.
  jmethodID g_mid_stop = nullptr;  ///< VideoBridge.stopEncoder()V.
  jmethodID g_mid_keyframe = nullptr;  ///< VideoBridge.requestKeyframe()V.
  jmethodID g_mid_bitrate = nullptr;  ///< VideoBridge.setBitrate(I)V.

  /**
   * @brief Get a JNIEnv for the current thread, attaching it to the VM if needed.
   * @details Upcalls run on native encoder threads that aren't attached to the JVM. The thread is
   *          left attached; it is detached implicitly when it exits.
   *
   * @return JNIEnv for the current thread, or nullptr when no VM is available.
   */
  JNIEnv *upcall_env() {
    if (!g_jvm) {
      return nullptr;
    }
    JNIEnv *env = nullptr;
    if (g_jvm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) == JNI_EDETACHED) {
      if (g_jvm->AttachCurrentThread(&env, nullptr) != 0) {
        return nullptr;
      }
    }
    return env;
  }
}  // namespace

extern "C" {

  /**
   * @brief Cache the Java VM when the library is loaded.
   *
   * @param vm The Java VM.
   * @return Supported JNI version.
   */
  JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *) {
    g_jvm = vm;
    jni::set_vm(vm, nullptr);
    return JNI_VERSION_1_6;
  }

  /**
   * @brief Boot the Sunshine core.
   * @note Blocks until Sunshine shuts down, so the caller must run this on a dedicated thread.
   *
   * @param env JNI environment.
   * @param data_dir App-private data directory holding config, state, and assets.
   * @return Sunshine's exit code.
   */
  JNIEXPORT jint JNICALL Java_dev_lizardbyte_sunshine_SunshineNative_nativeStart(JNIEnv *env, jclass, jstring data_dir) {
    const char *dir = env->GetStringUTFChars(data_dir, nullptr);
    if (dir) {
      setenv("SUNSHINE_APPDATA", dir, 1);
      env->ReleaseStringUTFChars(data_dir, dir);
    }

    char arg0[] = "libsunshine";
    char *argv[] = {arg0, nullptr};
    return main(1, argv);
  }

  /**
   * @brief Request a graceful shutdown of the Sunshine core.
   * @details Raises SIGINT, which Sunshine's handler turns into a clean shutdown, unblocking
   *          nativeStart().
   */
  JNIEXPORT void JNICALL Java_dev_lizardbyte_sunshine_SunshineNative_nativeStop(JNIEnv *, jclass) {
    raise(SIGINT);
  }

  /**
   * @brief Notify the core that MediaProjection capture has started at the given size.
   *
   * @param width Capture width in pixels.
   * @param height Capture height in pixels.
   */
  JNIEXPORT void JNICALL Java_dev_lizardbyte_sunshine_SunshineNative_nativeCaptureStarted(JNIEnv *, jclass, jint width, jint height) {
    jni::capture_started(width, height);
  }

  /**
   * @brief Notify the core that MediaProjection capture has stopped.
   */
  JNIEXPORT void JNICALL Java_dev_lizardbyte_sunshine_SunshineNative_nativeCaptureStopped(JNIEnv *, jclass) {
    jni::capture_stopped();
  }

  /**
   * @brief Deliver one captured RGBA_8888 frame from the app to the core.
   *
   * @param buffer Direct ByteBuffer holding the frame pixels.
   * @param width Frame width in pixels.
   * @param height Frame height in pixels.
   * @param row_stride Bytes per source row.
   */
  JNIEXPORT void JNICALL Java_dev_lizardbyte_sunshine_SunshineNative_nativePushFrame(JNIEnv *env, jclass, jobject buffer, jint width, jint height, jint row_stride) {
    auto *pixels = static_cast<const std::uint8_t *>(env->GetDirectBufferAddress(buffer));
    if (pixels) {
      jni::push_frame(pixels, width, height, row_stride);
    }
  }

  /**
   * @brief Notify the core that AudioPlaybackCapture has started.
   *
   * @param sample_rate Source sample rate in hertz.
   * @param channels Number of interleaved source channels.
   */
  JNIEXPORT void JNICALL Java_dev_lizardbyte_sunshine_SunshineNative_nativeAudioStarted(JNIEnv *, jclass, jint sample_rate, jint channels) {
    jni::audio_started(sample_rate, channels);
  }

  /**
   * @brief Notify the core that AudioPlaybackCapture has stopped.
   */
  JNIEXPORT void JNICALL Java_dev_lizardbyte_sunshine_SunshineNative_nativeAudioStopped(JNIEnv *, jclass) {
    jni::audio_stopped();
  }

  /**
   * @brief Deliver interleaved float PCM captured by the app to the core.
   *
   * @param buffer Direct ByteBuffer holding the float samples.
   * @param count Number of float samples (frames * source channels).
   */
  JNIEXPORT void JNICALL Java_dev_lizardbyte_sunshine_SunshineNative_nativePushAudio(JNIEnv *env, jclass, jobject buffer, jint count) {
    auto *samples = static_cast<const float *>(env->GetDirectBufferAddress(buffer));
    if (samples) {
      jni::push_audio(samples, count);
    }
  }

  /**
   * @brief Register (or clear) the Shizuku IInputBridge object for non-root input injection.
   * @details The bridge object (IInputBridge.Stub implementation) lives in the Shizuku UserService
   *          process; calls go over Binder IPC and execute under the shell SELinux context which
   *          holds android.permission.INJECT_EVENTS.
   *
   * @param bridge The IInputBridge AIDL stub, or null to disable non-root input.
   */
  JNIEXPORT void JNICALL Java_dev_lizardbyte_sunshine_SunshineNative_nativeSetInputBridge(JNIEnv *env, jclass, jobject bridge) {
    static jobject g_input_bridge = nullptr;
    static jmethodID g_mid_inject_key = nullptr;
    static jmethodID g_mid_mouse_move = nullptr;
    static jmethodID g_mid_mouse_btn = nullptr;
    static jmethodID g_mid_scroll = nullptr;
    static jmethodID g_mid_hscroll = nullptr;
    static jmethodID g_mid_alloc_gp = nullptr;
    static jmethodID g_mid_free_gp = nullptr;
    static jmethodID g_mid_inject_gp = nullptr;

    if (g_input_bridge) {
      env->DeleteGlobalRef(g_input_bridge);
      g_input_bridge = nullptr;
    }
    if (!bridge) {
      jni::set_input_callbacks({});
      return;
    }

    g_input_bridge = env->NewGlobalRef(bridge);
    jclass cls = env->GetObjectClass(bridge);
    g_mid_inject_key = env->GetMethodID(cls, "injectKey", "(IZ)V");
    g_mid_mouse_move = env->GetMethodID(cls, "injectMouseMove", "(FF)V");
    g_mid_mouse_btn  = env->GetMethodID(cls, "injectMouseButton", "(IZ)V");
    g_mid_scroll     = env->GetMethodID(cls, "injectScroll", "(I)V");
    g_mid_hscroll    = env->GetMethodID(cls, "injectHScroll", "(I)V");
    g_mid_alloc_gp   = env->GetMethodID(cls, "allocGamepad", "(I)V");
    g_mid_free_gp    = env->GetMethodID(cls, "freeGamepad", "(I)V");
    g_mid_inject_gp  = env->GetMethodID(cls, "injectGamepadState", "(IIFFFFFF)V");

    jni::input_callbacks_t cb;
    cb.inject_key = [](int vk, bool pressed) {
      JNIEnv *e = upcall_env();
      if (e && g_input_bridge && g_mid_inject_key) {
        e->CallVoidMethod(g_input_bridge, g_mid_inject_key, vk, static_cast<jboolean>(pressed));
      }
    };
    cb.inject_mouse_move = [](float dx, float dy) {
      JNIEnv *e = upcall_env();
      if (e && g_input_bridge && g_mid_mouse_move) {
        e->CallVoidMethod(g_input_bridge, g_mid_mouse_move, static_cast<jfloat>(dx), static_cast<jfloat>(dy));
      }
    };
    cb.inject_mouse_button = [](int button, bool pressed) {
      JNIEnv *e = upcall_env();
      if (e && g_input_bridge && g_mid_mouse_btn) {
        e->CallVoidMethod(g_input_bridge, g_mid_mouse_btn, button, static_cast<jboolean>(pressed));
      }
    };
    cb.inject_scroll = [](int distance) {
      JNIEnv *e = upcall_env();
      if (e && g_input_bridge && g_mid_scroll) {
        e->CallVoidMethod(g_input_bridge, g_mid_scroll, distance);
      }
    };
    cb.inject_hscroll = [](int distance) {
      JNIEnv *e = upcall_env();
      if (e && g_input_bridge && g_mid_hscroll) {
        e->CallVoidMethod(g_input_bridge, g_mid_hscroll, distance);
      }
    };
    cb.alloc_gamepad = [](int slot) {
      JNIEnv *e = upcall_env();
      if (e && g_input_bridge && g_mid_alloc_gp) {
        e->CallVoidMethod(g_input_bridge, g_mid_alloc_gp, slot);
      }
    };
    cb.free_gamepad = [](int slot) {
      JNIEnv *e = upcall_env();
      if (e && g_input_bridge && g_mid_free_gp) {
        e->CallVoidMethod(g_input_bridge, g_mid_free_gp, slot);
      }
    };
    cb.inject_gamepad = [](int slot, int buttons, float lx, float ly, float rx, float ry, float lt, float rt) {
      JNIEnv *e = upcall_env();
      if (e && g_input_bridge && g_mid_inject_gp) {
        e->CallVoidMethod(g_input_bridge, g_mid_inject_gp, slot, buttons,
          static_cast<jfloat>(lx), static_cast<jfloat>(ly),
          static_cast<jfloat>(rx), static_cast<jfloat>(ry),
          static_cast<jfloat>(lt), static_cast<jfloat>(rt));
      }
    };
    jni::set_input_callbacks(std::move(cb));
  }

  /**
   * @brief Register (or clear) the app object that services MediaCodec hardware encode.
   * @details The bridge must expose startEncoder(int,int,int,int,int):bool, stopEncoder(),
   *          requestKeyframe(), and setBitrate(int). Native encoder sessions call these via JNI.
   *
   * @param bridge The app-side bridge object, or null to disable hardware encode.
   */
  JNIEXPORT void JNICALL Java_dev_lizardbyte_sunshine_SunshineNative_nativeSetVideoBridge(JNIEnv *env, jclass, jobject bridge) {
    if (g_video_bridge) {
      env->DeleteGlobalRef(g_video_bridge);
      g_video_bridge = nullptr;
    }
    if (!bridge) {
      jni::set_video_callbacks({});
      return;
    }

    g_video_bridge = env->NewGlobalRef(bridge);
    jclass cls = env->GetObjectClass(bridge);
    g_mid_start = env->GetMethodID(cls, "startEncoder", "(IIIII)Z");
    g_mid_stop = env->GetMethodID(cls, "stopEncoder", "()V");
    g_mid_keyframe = env->GetMethodID(cls, "requestKeyframe", "()V");
    g_mid_bitrate = env->GetMethodID(cls, "setBitrate", "(I)V");

    jni::video_callbacks_t cb;
    cb.start = [](int w, int h, int codec, int fps, int bitrate) -> bool {
      JNIEnv *e = upcall_env();
      if (!e || !g_video_bridge || !g_mid_start) {
        return false;
      }
      return e->CallBooleanMethod(g_video_bridge, g_mid_start, w, h, codec, fps, bitrate) == JNI_TRUE;
    };
    cb.stop = [] {
      JNIEnv *e = upcall_env();
      if (e && g_video_bridge && g_mid_stop) {
        e->CallVoidMethod(g_video_bridge, g_mid_stop);
      }
    };
    cb.request_keyframe = [] {
      JNIEnv *e = upcall_env();
      if (e && g_video_bridge && g_mid_keyframe) {
        e->CallVoidMethod(g_video_bridge, g_mid_keyframe);
      }
    };
    cb.set_bitrate = [](int bitrate) {
      JNIEnv *e = upcall_env();
      if (e && g_video_bridge && g_mid_bitrate) {
        e->CallVoidMethod(g_video_bridge, g_mid_bitrate, bitrate);
      }
    };
    jni::set_video_callbacks(std::move(cb));
  }

  /**
   * @brief Deliver one encoded access unit from the app's MediaCodec output thread.
   *
   * @param buffer Direct ByteBuffer holding the encoded bytes.
   * @param size Number of encoded bytes.
   * @param flags Bit 0: keyframe. Bit 1: codec config.
   * @param pts_us Presentation timestamp in microseconds.
   */
  JNIEXPORT void JNICALL Java_dev_lizardbyte_sunshine_SunshineNative_nativePushEncoded(JNIEnv *env, jclass, jobject buffer, jint size, jint flags, jlong pts_us) {
    auto *data = static_cast<const std::uint8_t *>(env->GetDirectBufferAddress(buffer));
    if (data) {
      jni::push_encoded(data, size, flags, pts_us);
    }
  }
}
