/**
 * @file src/platform/android/input.cpp
 * @brief Android input-injection backend (rooted).
 * @details Creates virtual mouse and keyboard devices through the kernel `/dev/uinput` facility.
 *          Android's input stack (EventHub -> InputReader) reads `/dev/input/event*`, so a uinput
 *          device we create is picked up like a real mouse/keyboard. The keycode table mirrors the
 *          Linux inputtino backend (Android uses the same evdev KEY_* codes).
 *
 *          Requires root and, typically, a permissive SELinux context for `/dev/uinput`
 *          (`setenforce 0` on a test device if device creation is denied). The mouse is a
 *          relative device; absolute moves are converted to relative deltas via a tracked cursor
 *          position (subject to pointer acceleration — prefer Moonlight's relative mouse mode).
 *          Touch, pen, and gamepad remain unimplemented.
 */
// standard includes
#include <cmath>
#include <cstring>
#include <map>
#include <vector>

// platform includes
#include <fcntl.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <unistd.h>

// local includes
#include "src/platform/android/jni_bridge.h"
#include "src/platform/android/misc.h"
#include "src/platform/common.h"
#include "src/logging.h"

using namespace std::literals;

namespace platf {

  namespace {
    /**
     * @brief Map of Moonlight (Windows VK) key code to Linux evdev KEY_* code.
     * @details Reverse of the Linux inputtino table; Android uses identical evdev codes.
     */
    const std::map<std::uint16_t, int> vk_to_linux = {
      {0x08, KEY_BACKSPACE}, {0x09, KEY_TAB}, {0x0D, KEY_ENTER}, {0x10, KEY_LEFTSHIFT},
      {0x11, KEY_LEFTCTRL}, {0x12, KEY_LEFTALT}, {0x14, KEY_CAPSLOCK}, {0x1B, KEY_ESC},
      {0x20, KEY_SPACE}, {0x21, KEY_PAGEUP}, {0x22, KEY_PAGEDOWN}, {0x23, KEY_END},
      {0x24, KEY_HOME}, {0x25, KEY_LEFT}, {0x26, KEY_UP}, {0x27, KEY_RIGHT}, {0x28, KEY_DOWN},
      {0x2C, KEY_SYSRQ}, {0x2D, KEY_INSERT}, {0x2E, KEY_DELETE},
      {0x30, KEY_0}, {0x31, KEY_1}, {0x32, KEY_2}, {0x33, KEY_3}, {0x34, KEY_4}, {0x35, KEY_5},
      {0x36, KEY_6}, {0x37, KEY_7}, {0x38, KEY_8}, {0x39, KEY_9},
      {0x41, KEY_A}, {0x42, KEY_B}, {0x43, KEY_C}, {0x44, KEY_D}, {0x45, KEY_E}, {0x46, KEY_F},
      {0x47, KEY_G}, {0x48, KEY_H}, {0x49, KEY_I}, {0x4A, KEY_J}, {0x4B, KEY_K}, {0x4C, KEY_L},
      {0x4D, KEY_M}, {0x4E, KEY_N}, {0x4F, KEY_O}, {0x50, KEY_P}, {0x51, KEY_Q}, {0x52, KEY_R},
      {0x53, KEY_S}, {0x54, KEY_T}, {0x55, KEY_U}, {0x56, KEY_V}, {0x57, KEY_W}, {0x58, KEY_X},
      {0x59, KEY_Y}, {0x5A, KEY_Z}, {0x5B, KEY_LEFTMETA}, {0x5C, KEY_RIGHTMETA},
      {0x60, KEY_KP0}, {0x61, KEY_KP1}, {0x62, KEY_KP2}, {0x63, KEY_KP3}, {0x64, KEY_KP4},
      {0x65, KEY_KP5}, {0x66, KEY_KP6}, {0x67, KEY_KP7}, {0x68, KEY_KP8}, {0x69, KEY_KP9},
      {0x6A, KEY_KPASTERISK}, {0x6B, KEY_KPPLUS}, {0x6D, KEY_KPMINUS}, {0x6E, KEY_KPDOT},
      {0x6F, KEY_KPSLASH},
      {0x70, KEY_F1}, {0x71, KEY_F2}, {0x72, KEY_F3}, {0x73, KEY_F4}, {0x74, KEY_F5},
      {0x75, KEY_F6}, {0x76, KEY_F7}, {0x77, KEY_F8}, {0x78, KEY_F9}, {0x79, KEY_F10},
      {0x7A, KEY_F11}, {0x7B, KEY_F12},
      {0x90, KEY_NUMLOCK}, {0x91, KEY_SCROLLLOCK},
      {0xA0, KEY_LEFTSHIFT}, {0xA1, KEY_RIGHTSHIFT}, {0xA2, KEY_LEFTCTRL}, {0xA3, KEY_RIGHTCTRL},
      {0xA4, KEY_LEFTALT}, {0xA5, KEY_RIGHTALT},
      {0xBA, KEY_SEMICOLON}, {0xBB, KEY_EQUAL}, {0xBC, KEY_COMMA}, {0xBD, KEY_MINUS},
      {0xBE, KEY_DOT}, {0xBF, KEY_SLASH}, {0xC0, KEY_GRAVE}, {0xDB, KEY_LEFTBRACE},
      {0xDC, KEY_BACKSLASH}, {0xDD, KEY_RIGHTBRACE}, {0xDE, KEY_APOSTROPHE}, {0xE2, KEY_102ND},
    };

    /**
     * @brief Write a single input event to a uinput fd.
     *
     * @param fd Target uinput file descriptor.
     * @param type Event type (EV_KEY, EV_REL, EV_SYN, ...).
     * @param code Event code within the type.
     * @param value Event value.
     */
    void emit(int fd, std::uint16_t type, std::uint16_t code, std::int32_t value) {
      if (fd < 0) {
        return;
      }
      input_event ev {};
      ev.type = type;
      ev.code = code;
      ev.value = value;
      ssize_t written = write(fd, &ev, sizeof(ev));
      (void) written;
    }

    /**
     * @brief Emit a SYN_REPORT to flush queued events.
     *
     * @param fd Target uinput file descriptor.
     */
    void emit_syn(int fd) {
      emit(fd, EV_SYN, SYN_REPORT, 0);
    }

    /**
     * @brief Create a uinput device using the legacy (widely compatible) ABI.
     *
     * @param name Human-readable device name.
     * @param product USB product id used to distinguish the virtual devices.
     * @param setup Callback that enables the required event/code bits on the fd.
     * @param bustype BUS_USB for external-class devices, BUS_VIRTUAL for internal-class devices.
     * @return Open uinput fd on success, or -1 on failure.
     */
    template<class FN>
    int create_uinput(const char *name, std::uint16_t product, FN &&setup, std::uint16_t bustype = BUS_USB) {
      int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
      if (fd < 0) {
        BOOST_LOG(warning) << "android input: cannot open /dev/uinput ("sv << std::strerror(errno)
                           << "). Root and a permissive SELinux context are required."sv;
        return -1;
      }

      setup(fd);

      uinput_user_dev uud {};
      std::strncpy(uud.name, name, UINPUT_MAX_NAME_SIZE - 1);
      uud.id.bustype = bustype;
      uud.id.vendor = 0x1209;  // pid.codes open-source VID
      uud.id.product = product;
      uud.id.version = 1;

      if (write(fd, &uud, sizeof(uud)) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
        BOOST_LOG(warning) << "android input: failed to create uinput device '"sv << name << "': "sv << std::strerror(errno);
        close(fd);
        return -1;
      }
      return fd;
    }

    /**
     * @brief Convenience accessor for the backend state from an input handle.
     *
     * @param input Platform input handle.
     * @return Pointer to the Android input state, or nullptr.
     */
    android_input_t *raw(input_t &input) {
      return static_cast<android_input_t *>(input.get());
    }
  }  // namespace

  int android_create_uinput_mouse() {
    return create_uinput("Sunshine Mouse", 0x0001, [](int fd) {
      ioctl(fd, UI_SET_EVBIT, EV_KEY);
      for (int btn : {BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_SIDE, BTN_EXTRA}) {
        ioctl(fd, UI_SET_KEYBIT, btn);
      }
      ioctl(fd, UI_SET_EVBIT, EV_REL);
      for (int rel : {REL_X, REL_Y, REL_WHEEL, REL_HWHEEL, REL_WHEEL_HI_RES, REL_HWHEEL_HI_RES}) {
        ioctl(fd, UI_SET_RELBIT, rel);
      }
    });
  }

  int android_create_uinput_keyboard() {
    // BUS_VIRTUAL: Android's EventHub only classifies BUS_USB and BUS_BLUETOOTH devices as
    // "external", and only external alphabetic keyboards suppress the IME. Using BUS_VIRTUAL
    // keeps the device "internal" so the soft keyboard still appears when text fields are focused.
    return create_uinput("Sunshine Keyboard", 0x0002, [](int fd) {
      ioctl(fd, UI_SET_EVBIT, EV_KEY);
      for (const auto &[vk, key] : vk_to_linux) {
        ioctl(fd, UI_SET_KEYBIT, key);
      }
    }, BUS_VIRTUAL);
  }

  void freeInput(void *p) {
    auto *state = static_cast<android_input_t *>(p);
    if (!state) {
      return;
    }
    for (int fd : {state->uinput_mouse_fd, state->uinput_keyboard_fd, state->uinput_touch_fd}) {
      if (fd >= 0) {
        ioctl(fd, UI_DEV_DESTROY);
        close(fd);
      }
    }
    delete state;
  }

  input_t input() {
    auto state = new android_input_t {};

    state->uinput_mouse_fd = android_create_uinput_mouse();
    state->uinput_keyboard_fd = android_create_uinput_keyboard();

    if (state->uinput_mouse_fd >= 0 || state->uinput_keyboard_fd >= 0) {
      BOOST_LOG(info) << "android input: uinput devices ready (mouse="sv << (state->uinput_mouse_fd >= 0)
                      << " keyboard="sv << (state->uinput_keyboard_fd >= 0) << ")"sv;
    } else {
      // uinput unavailable (non-root) — fall back to injectInputEvent via Shizuku AIDL.
      // Each handler checks the fd at call time, so late Shizuku bridge registration is fine.
      BOOST_LOG(info) << "android input: uinput unavailable; will use Shizuku event injection"sv;
    }
    return input_t {state};
  }

  util::point_t get_mouse_loc(input_t &input) {
    auto *state = raw(input);
    return {state->abs_x, state->abs_y};
  }

  void move_mouse(input_t &input, int deltaX, int deltaY) {
    auto *state = raw(input);
    state->abs_x += deltaX;
    state->abs_y += deltaY;
    if (state->uinput_mouse_fd >= 0) {
      emit(state->uinput_mouse_fd, EV_REL, REL_X, deltaX);
      emit(state->uinput_mouse_fd, EV_REL, REL_Y, deltaY);
      emit_syn(state->uinput_mouse_fd);
    } else {
      jni::inject_mouse_move(static_cast<float>(deltaX), static_cast<float>(deltaY));
    }
  }

  void abs_mouse(input_t &input, const touch_port_t &touch_port, float x, float y) {
    auto *state = raw(input);
    int deltaX = static_cast<int>(std::lround(x - state->abs_x));
    int deltaY = static_cast<int>(std::lround(y - state->abs_y));
    state->abs_x = x;
    state->abs_y = y;
    if (state->uinput_mouse_fd >= 0) {
      emit(state->uinput_mouse_fd, EV_REL, REL_X, deltaX);
      emit(state->uinput_mouse_fd, EV_REL, REL_Y, deltaY);
      emit_syn(state->uinput_mouse_fd);
    } else {
      jni::inject_mouse_move(static_cast<float>(deltaX), static_cast<float>(deltaY));
    }
  }

  void button_mouse(input_t &input, int button, bool release) {
    auto *state = raw(input);
    if (state->uinput_mouse_fd >= 0) {
      int btn;
      switch (button) {
        case BUTTON_LEFT:   btn = BTN_LEFT;   break;
        case BUTTON_MIDDLE: btn = BTN_MIDDLE;  break;
        case BUTTON_RIGHT:  btn = BTN_RIGHT;  break;
        case BUTTON_X1:     btn = BTN_SIDE;   break;
        case BUTTON_X2:     btn = BTN_EXTRA;  break;
        default:
          BOOST_LOG(warning) << "android input: unknown mouse button "sv << button;
          return;
      }
      emit(state->uinput_mouse_fd, EV_KEY, btn, release ? 0 : 1);
      emit_syn(state->uinput_mouse_fd);
    } else {
      int jni_btn = 0;
      switch (button) {
        case BUTTON_LEFT:   jni_btn = 1;  break;
        case BUTTON_RIGHT:  jni_btn = 2;  break;
        case BUTTON_MIDDLE: jni_btn = 4;  break;
        case BUTTON_X1:     jni_btn = 8;  break;
        case BUTTON_X2:     jni_btn = 16; break;
        default: return;
      }
      jni::inject_mouse_button(jni_btn, !release);
    }
  }

  void scroll(input_t &input, int distance) {
    auto *state = raw(input);
    if (state->uinput_mouse_fd >= 0) {
      // Moonlight sends high-resolution scroll in 1/120 units (120 == one notch).
      emit(state->uinput_mouse_fd, EV_REL, REL_WHEEL_HI_RES, distance);
      emit(state->uinput_mouse_fd, EV_REL, REL_WHEEL, static_cast<int>(std::lround(distance / 120.0)));
      emit_syn(state->uinput_mouse_fd);
    } else {
      jni::inject_scroll(distance);
    }
  }

  void hscroll(input_t &input, int distance) {
    auto *state = raw(input);
    if (state->uinput_mouse_fd >= 0) {
      emit(state->uinput_mouse_fd, EV_REL, REL_HWHEEL_HI_RES, distance);
      emit(state->uinput_mouse_fd, EV_REL, REL_HWHEEL, static_cast<int>(std::lround(distance / 120.0)));
      emit_syn(state->uinput_mouse_fd);
    } else {
      jni::inject_hscroll(distance);
    }
  }

  void keyboard_update(input_t &input, uint16_t modcode, bool release, uint8_t flags) {
    auto *state = raw(input);
    if (state->uinput_keyboard_fd >= 0) {
      auto it = vk_to_linux.find(modcode);
      if (it == vk_to_linux.end()) {
        BOOST_LOG(debug) << "android input: unmapped key code 0x"sv << std::hex << modcode << std::dec;
        return;
      }
      emit(state->uinput_keyboard_fd, EV_KEY, it->second, release ? 0 : 1);
      emit_syn(state->uinput_keyboard_fd);
    } else {
      jni::inject_key(static_cast<int>(modcode), !release);
    }
  }

  void unicode(input_t &input, char *utf8, int size) {
    // Direct Unicode text entry has no portable uinput mechanism on Android; skip for now.
    // Regular key events go through keyboard_update().
  }

  void gamepad_update(input_t &input, int nr, const gamepad_state_t &gamepad_state) {
    jni::inject_gamepad(nr,
      static_cast<int>(gamepad_state.buttonFlags),
      gamepad_state.lsX / 32767.f,
      gamepad_state.lsY / 32767.f,
      gamepad_state.rsX / 32767.f,
      gamepad_state.rsY / 32767.f,
      gamepad_state.lt / 255.f,
      gamepad_state.rt / 255.f);
  }

  std::unique_ptr<client_input_t> allocate_client_input_context(input_t &input) {
    return std::make_unique<client_input_t>();
  }

  void touch_update(client_input_t *input, const touch_port_t &touch_port, const touch_input_t &touch) {
    // TODO: multitouch protocol B via a virtual touchscreen (ABS_MT_*).
  }

  void pen_update(client_input_t *input, const touch_port_t &touch_port, const pen_input_t &pen) {
    // TODO: BTN_TOOL_PEN + ABS_PRESSURE/ABS_TILT on a virtual tablet.
  }

  void gamepad_touch(input_t &input, const gamepad_touch_t &touch) {}

  void gamepad_motion(input_t &input, const gamepad_motion_t &motion) {}

  void gamepad_battery(input_t &input, const gamepad_battery_t &battery) {}

  int alloc_gamepad(input_t &input, const gamepad_id_t &id, const gamepad_arrival_t &metadata, feedback_queue_t feedback_queue) {
    jni::alloc_gamepad(id.globalIndex);
    return 0;
  }

  void free_gamepad(input_t &input, int nr) {
    jni::free_gamepad(nr);
  }

  std::vector<supported_gamepad_t> &supported_gamepads(input_t *input) {
    static std::vector<supported_gamepad_t> gamepads {
      {"gamepad"s, true, ""s},
    };
    return gamepads;
  }

}  // namespace platf
