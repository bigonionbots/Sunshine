/**
 * @file src/platform/android/jni_bridge.cpp
 * @brief Headless-safe default implementations of the Android JNI bridge.
 * @details These stubs let the native core link and run without an attached JVM. As the host
 *          app lands, `set_vm()` will capture real handles and the getters will call into Java.
 */
// local includes
#include "src/platform/android/jni_bridge.h"

namespace jni {

  namespace {
    void *g_vm = nullptr;  ///< Cached JavaVM pointer, or null when running headless.
    void *g_app_context = nullptr;  ///< Cached global reference to the host Context.
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

}  // namespace jni
