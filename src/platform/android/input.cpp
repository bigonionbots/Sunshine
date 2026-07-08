/**
 * @file src/platform/android/input.cpp
 * @brief Android input-injection backend.
 * @details On a rooted device the intended implementation drives `/dev/uinput` virtual devices
 *          (the same mechanism the Linux backend's inputtino uses), which lets injected events
 *          reach arbitrary apps. This file currently provides the full set of symbols as no-op
 *          stubs so the core links and boots; each function notes its intended uinput mapping.
 */
// standard includes
#include <vector>

// local includes
#include "src/platform/android/misc.h"
#include "src/platform/common.h"
#include "src/logging.h"

using namespace std::literals;

namespace platf {

  void freeInput(void *p) {
    delete static_cast<android_input_t *>(p);
  }

  input_t input() {
    // TODO (rooted): open /dev/uinput and create virtual mouse/keyboard/touch devices here.
    return input_t {new android_input_t {}};
  }

  util::point_t get_mouse_loc(input_t &input) {
    return {};
  }

  void move_mouse(input_t &input, int deltaX, int deltaY) {
    // TODO: EV_REL REL_X/REL_Y on the virtual mouse.
  }

  void abs_mouse(input_t &input, const touch_port_t &touch_port, float x, float y) {
    // TODO: EV_ABS ABS_X/ABS_Y on the virtual touchscreen/tablet.
  }

  void button_mouse(input_t &input, int button, bool release) {
    // TODO: EV_KEY BTN_LEFT/BTN_RIGHT/BTN_MIDDLE.
  }

  void scroll(input_t &input, int distance) {
    // TODO: EV_REL REL_WHEEL / REL_WHEEL_HI_RES.
  }

  void hscroll(input_t &input, int distance) {
    // TODO: EV_REL REL_HWHEEL / REL_HWHEEL_HI_RES.
  }

  void keyboard_update(input_t &input, uint16_t modcode, bool release, uint8_t flags) {
    // TODO: map Moonlight keycode to Linux KEY_* and emit EV_KEY.
  }

  void unicode(input_t &input, char *utf8, int size) {
    // TODO: synthesize key events for the provided UTF-8 text.
  }

  void gamepad_update(input_t &input, int nr, const gamepad_state_t &gamepad_state) {
    // TODO: drive a uinput virtual gamepad (ABS axes + BTN_ buttons).
  }

  std::unique_ptr<client_input_t> allocate_client_input_context(input_t &input) {
    return std::make_unique<client_input_t>();
  }

  void touch_update(client_input_t *input, const touch_port_t &touch_port, const touch_input_t &touch) {
    // TODO: multitouch protocol B via the virtual touchscreen (ABS_MT_*).
  }

  void pen_update(client_input_t *input, const touch_port_t &touch_port, const pen_input_t &pen) {
    // TODO: BTN_TOOL_PEN + ABS_PRESSURE/ABS_TILT on a virtual tablet.
  }

  void gamepad_touch(input_t &input, const gamepad_touch_t &touch) {}

  void gamepad_motion(input_t &input, const gamepad_motion_t &motion) {}

  void gamepad_battery(input_t &input, const gamepad_battery_t &battery) {}

  int alloc_gamepad(input_t &input, const gamepad_id_t &id, const gamepad_arrival_t &metadata, feedback_queue_t feedback_queue) {
    // TODO: create a virtual gamepad; return 0 on success.
    return -1;
  }

  void free_gamepad(input_t &input, int nr) {}

  std::vector<supported_gamepad_t> &supported_gamepads(input_t *input) {
    static std::vector<supported_gamepad_t> gamepads {
      {"gamepad"s, false, "not yet implemented on android"s},
    };
    return gamepads;
  }

}  // namespace platf
