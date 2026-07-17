/**
 * @file src/platform/android/uinput_helper.cpp
 * @brief Tiny JNI library (libsunshine_input.so) for uinput device lifecycle and screencap daemon.
 * @details Loaded by the Shizuku UserService running as shell so it can open /dev/uinput and
 *          create real kernel input devices (mouse cursor visible, gamepad recognized by games),
 *          and run a persistent screencap loop that streams raw frames over a pipe without
 *          spawning a new process per frame. Intentionally has NO dependency on libsunshine.so
 *          or its heavy static initializers.
 */
// standard includes
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

// platform includes
#include <fcntl.h>
#include <jni.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <pthread.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Persistent screencap daemon
// ---------------------------------------------------------------------------

namespace {
  std::atomic<bool> g_daemon_stop {false};  ///< Set true to signal the daemon to exit.
  pthread_t g_daemon_thread {};             ///< Handle to the running daemon thread (or 0).
  std::atomic<bool> g_daemon_running {false};  ///< True while the thread is alive.

  // Transfer buffer between screencap stdout and the write-fd; lives in BSS.
  static const std::size_t COPY_BUF_SIZE = 65536;
  static std::uint8_t g_copy_buf[COPY_BUF_SIZE];

  /**
   * @brief Daemon thread: loop popen(screencap) and stream RGBA frames to a pipe.
   * @details Each frame is prefixed by an 8-byte header [width(LE-int32), height(LE-int32)]
   *          followed by width*height*4 raw RGBA_8888 bytes from screencap.  The loop runs
   *          until g_daemon_stop is set or a write to the pipe fails (broken pipe).
   *
   * @param arg Write-end fd (as intptr_t).
   * @return Always nullptr.
   */
  void *screencap_daemon_func(void *arg) {
    int write_fd = static_cast<int>(reinterpret_cast<std::intptr_t>(arg));

    // Ignore SIGPIPE so write() returns EPIPE instead of killing the process when the
    // read-end of the pipe is closed by the SunshineService.
    signal(SIGPIPE, SIG_IGN);

    while (!g_daemon_stop.load(std::memory_order_relaxed)) {
      FILE *f = popen("/system/bin/screencap", "r");
      if (!f) {
        continue;
      }

      // Read 16 bytes: on Android 12+ screencap writes [uint32 w, uint32 h, uint32 format,
      // uint32 colorspace]; on older versions it writes [uint32 format, uint32 w, uint32 h].
      // We always treat bytes 0-3 as width and 4-7 as height, matching the Kotlin reader.
      std::uint8_t sc_hdr[16];
      std::size_t hdr_read = std::fread(sc_hdr, 1, 16, f);
      if (hdr_read < 8) {
        pclose(f);
        continue;
      }

      std::uint32_t cap_w, cap_h;
      std::memcpy(&cap_w, sc_hdr + 0, 4);
      std::memcpy(&cap_h, sc_hdr + 4, 4);

      if (cap_w == 0 || cap_h == 0 || cap_w > 8192 || cap_h > 8192) {
        pclose(f);
        continue;
      }

      std::size_t pixel_bytes = static_cast<std::size_t>(cap_w) * cap_h * 4;

      // Write our compact 8-byte header to the pipe.
      std::uint8_t out_hdr[8];
      std::memcpy(out_hdr + 0, &cap_w, 4);
      std::memcpy(out_hdr + 4, &cap_h, 4);
      {
        const std::uint8_t *p = out_hdr;
        std::size_t rem = 8;
        while (rem > 0) {
          ssize_t nw = write(write_fd, p, rem);
          if (nw <= 0) { pclose(f); f = nullptr; break; }
          p += nw; rem -= static_cast<std::size_t>(nw);
        }
      }
      if (!f) { break; }  // write error: pipe closed on reader side

      // Stream pixel data from screencap stdout to the write-fd in chunks.
      std::size_t remaining = pixel_bytes;
      bool write_ok = true;
      while (remaining > 0 && !g_daemon_stop.load(std::memory_order_relaxed)) {
        std::size_t to_read = remaining < COPY_BUF_SIZE ? remaining : COPY_BUF_SIZE;
        std::size_t nr = std::fread(g_copy_buf, 1, to_read, f);
        if (nr == 0) { break; }
        const std::uint8_t *p = g_copy_buf;
        std::size_t nw_rem = nr;
        while (nw_rem > 0) {
          ssize_t nw = write(write_fd, p, nw_rem);
          if (nw <= 0) { write_ok = false; break; }
          p += nw; nw_rem -= static_cast<std::size_t>(nw);
        }
        if (!write_ok) { break; }
        remaining -= nr;
      }
      pclose(f);
      if (!write_ok) { break; }
    }

    close(write_fd);
    g_daemon_running.store(false);
    return nullptr;
  }
}  // namespace

extern "C" {

  /**
   * @brief Start the persistent screencap daemon thread.
   * @details Stops any previously running daemon, then launches a new thread that continuously
   *          runs /system/bin/screencap and streams RGBA frames to [write_fd].  The fd is
   *          owned by the thread after this call returns; the caller must not close it.
   *
   * @param write_fd Write-end of a pipe created by the caller; ownership transferred to daemon.
   */
  JNIEXPORT void JNICALL
  Java_dev_lizardbyte_sunshine_SunshineInputNative_nativeStartScreencapDaemon(JNIEnv *, jclass, jint write_fd) {
    // Stop any existing daemon and wait for it to exit.
    if (g_daemon_running.load()) {
      g_daemon_stop.store(true);
      pthread_join(g_daemon_thread, nullptr);
      g_daemon_thread = {};
    }
    g_daemon_stop.store(false);
    g_daemon_running.store(true);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    if (pthread_create(&g_daemon_thread, &attr,
                       screencap_daemon_func,
                       reinterpret_cast<void *>(static_cast<std::intptr_t>(static_cast<int>(write_fd)))) != 0) {
      g_daemon_running.store(false);
      close(write_fd);
    }
    pthread_attr_destroy(&attr);
  }

  /**
   * @brief Signal the screencap daemon to stop after its current frame.
   * @details Returns immediately; the daemon thread exits asynchronously once the in-progress
   *          screencap process finishes.  Closing the read-end of the pipe also forces exit.
   */
  JNIEXPORT void JNICALL
  Java_dev_lizardbyte_sunshine_SunshineInputNative_nativeStopScreencapDaemon(JNIEnv *, jclass) {
    g_daemon_stop.store(true);
  }

  /**
   * @brief Create a uinput virtual mouse with relative axes and all standard buttons.
   *
   * @return Open uinput fd on success, -1 on failure.
   */
  JNIEXPORT jint JNICALL
  Java_dev_lizardbyte_sunshine_SunshineInputNative_createUinputMouse(JNIEnv *, jclass) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
      return -1;
    }
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    for (int b : {BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_SIDE, BTN_EXTRA}) {
      ioctl(fd, UI_SET_KEYBIT, b);
    }
    ioctl(fd, UI_SET_EVBIT, EV_REL);
    for (int r : {REL_X, REL_Y, REL_WHEEL, REL_HWHEEL, REL_WHEEL_HI_RES, REL_HWHEEL_HI_RES}) {
      ioctl(fd, UI_SET_RELBIT, r);
    }
    uinput_user_dev uud {};
    std::strncpy(uud.name, "Sunshine Mouse", UINPUT_MAX_NAME_SIZE - 1);
    uud.id.bustype = BUS_USB;
    uud.id.vendor = 0x1209;
    uud.id.product = 0x0001;
    uud.id.version = 1;
    if (write(fd, &uud, sizeof(uud)) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
      close(fd);
      return -1;
    }
    return fd;
  }

  /**
   * @brief Create a uinput virtual keyboard covering the full HID key range.
   *
   * @return Open uinput fd on success, -1 on failure.
   */
  JNIEXPORT jint JNICALL
  Java_dev_lizardbyte_sunshine_SunshineInputNative_createUinputKeyboard(JNIEnv *, jclass) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
      return -1;
    }
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    for (int k = 1; k < KEY_MAX; ++k) {
      ioctl(fd, UI_SET_KEYBIT, k);
    }
    uinput_user_dev uud {};
    std::strncpy(uud.name, "Sunshine Keyboard", UINPUT_MAX_NAME_SIZE - 1);
    // BUS_VIRTUAL prevents Android's EventHub from classifying this as an external hard keyboard,
    // which would otherwise suppress the soft keyboard (IME) when text fields are focused.
    uud.id.bustype = BUS_VIRTUAL;
    uud.id.vendor = 0x1209;
    uud.id.product = 0x0002;
    uud.id.version = 1;
    if (write(fd, &uud, sizeof(uud)) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
      close(fd);
      return -1;
    }
    return fd;
  }

  /**
   * @brief Create a uinput virtual gamepad (Xbox-style: dual sticks, triggers, hat, 11 buttons).
   *
   * @param slot Gamepad slot index (0–3); used in the device name and USB product ID.
   * @return Open uinput fd on success, -1 on failure.
   */
  JNIEXPORT jint JNICALL
  Java_dev_lizardbyte_sunshine_SunshineInputNative_createUinputGamepad(JNIEnv *, jclass, jint slot) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
      return -1;
    }
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    for (int b : {BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_WEST,
                  BTN_TL, BTN_TR, BTN_SELECT, BTN_START, BTN_MODE,
                  BTN_THUMBL, BTN_THUMBR}) {
      ioctl(fd, UI_SET_KEYBIT, b);
    }
    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    for (int a : {ABS_X, ABS_Y, ABS_RX, ABS_RY, ABS_Z, ABS_RZ, ABS_HAT0X, ABS_HAT0Y}) {
      ioctl(fd, UI_SET_ABSBIT, a);
    }
    uinput_user_dev uud {};
    std::snprintf(uud.name, UINPUT_MAX_NAME_SIZE, "Sunshine Gamepad %d", static_cast<int>(slot));
    // Microsoft Xbox One controller VID/PID. Android ships a keylayout for this device
    // (Vendor_045e_Product_02dd.kl) that maps BTN_SOUTH→BUTTON_A etc., and Chrome applies
    // the Xbox standard-gamepad mapping so RS lands at axes[2]/[3] and triggers as buttons.
    uud.id.bustype = BUS_USB;
    uud.id.vendor = 0x045E;
    uud.id.product = static_cast<uint16_t>(0x02DD + slot);
    uud.id.version = 1;
    // Dual sticks: ±32767
    for (int a : {ABS_X, ABS_Y, ABS_RX, ABS_RY}) {
      uud.absmin[a] = -32767;
      uud.absmax[a] = 32767;
      uud.absfuzz[a] = 16;
      uud.absflat[a] = 128;
    }
    // Analog triggers: 0–255
    for (int a : {ABS_Z, ABS_RZ}) {
      uud.absmin[a] = 0;
      uud.absmax[a] = 255;
    }
    // D-pad hat: ±1
    for (int a : {ABS_HAT0X, ABS_HAT0Y}) {
      uud.absmin[a] = -1;
      uud.absmax[a] = 1;
    }
    if (write(fd, &uud, sizeof(uud)) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
      close(fd);
      return -1;
    }
    return fd;
  }

  /**
   * @brief Destroy a uinput device and close its fd.
   *
   * @param fd Open uinput fd returned by one of the create functions.
   */
  JNIEXPORT void JNICALL
  Java_dev_lizardbyte_sunshine_SunshineInputNative_destroyUinputDevice(JNIEnv *, jclass, jint fd) {
    if (fd >= 0) {
      ioctl(fd, UI_DEV_DESTROY);
      close(fd);
    }
  }

  /**
   * @brief Write one evdev event to an open uinput fd.
   * @details Used by the Shizuku UserService to stream input events directly, bypassing
   *          InputManager.injectInputEvent() and giving the kernel full control of pointer tracking.
   *
   * @param fd Open uinput fd.
   * @param type evdev event type (EV_KEY, EV_REL, EV_ABS, EV_SYN).
   * @param code Event code within the type.
   * @param value Event value.
   */
  JNIEXPORT void JNICALL
  Java_dev_lizardbyte_sunshine_SunshineInputNative_writeUinputEvent(JNIEnv *, jclass, jint fd, jshort type, jshort code, jint value) {
    if (fd < 0) {
      return;
    }
    struct input_event ev {};
    ev.type = static_cast<std::uint16_t>(type);
    ev.code = static_cast<std::uint16_t>(code);
    ev.value = value;
    write(fd, &ev, sizeof(ev));
  }

}  // extern "C"
