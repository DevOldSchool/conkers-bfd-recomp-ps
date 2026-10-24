// No input device: the null renderer has no window to take keyboard focus.
#include "platform/input.hpp"

namespace conker::input {

void read(uint16_t* buttons, float* x, float* y, int /*port*/) {
    *buttons = 0;
    *x = 0.0f;
    *y = 0.0f;
}

bool connected(int port) { return port == 0; }

std::string controller_name() { return ""; }

PadKind controller_kind() { return PadKind::None; }

bool take_debug_menu_press() { return false; }

bool has_rumble(int /*port*/) { return false; }
void set_rumble(int /*port*/, bool /*on*/) {}

bool has_haptics(int /*port*/) { return false; }
void play_haptic(const Haptic& /*haptic*/, int /*port*/) {}

bool has_adaptive_triggers() { return false; }
void set_trigger_effect(const TriggerEffect& /*effect*/) {}

bool camera_swapped() { return true; }
void set_camera_swapped(bool /*swapped*/) {}

} // namespace conker::input
