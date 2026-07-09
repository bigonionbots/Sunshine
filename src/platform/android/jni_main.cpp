/**
 * @file src/platform/android/jni_main.cpp
 * @brief JNI entry point for hosting the Sunshine core inside the Android app.
 * @details The app loads libsunshine.so and calls nativeStart() on a background thread. This
 *          points Sunshine's data directory at the app's private storage and then runs the same
 *          main() the standalone binary uses, which boots the servers and blocks until shutdown.
 */
// standard includes
#include <csignal>
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

extern "C" {

  /**
   * @brief Cache the Java VM when the library is loaded.
   *
   * @param vm The Java VM.
   * @return Supported JNI version.
   */
  JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *) {
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
}
