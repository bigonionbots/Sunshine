/**
 * @file src/platform/android/uinput_helper.cpp
 * @brief Tiny JNI library (libsunshine_input.so) for uinput device lifecycle.
 * @details Loaded by the Shizuku UserService running as shell so it can open /dev/uinput and
 *          create real kernel input devices (mouse cursor visible, gamepad recognized by games).
 *          Intentionally has NO dependency on libsunshine.so or its heavy static initializers.
 */
// standard includes
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

// platform includes
#include <fcntl.h>
#include <jni.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <unistd.h>

extern "C" {

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
