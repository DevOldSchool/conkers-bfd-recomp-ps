// No input device: the null renderer has no window to take keyboard focus.
#include "platform/input.hpp"

namespace conker::input {

void read(uint16_t* buttons, float* x, float* y) {
    *buttons = 0;
    *x = 0.0f;
    *y = 0.0f;
}

std::string controller_name() { return ""; }

bool take_debug_menu_press() { return false; }

bool has_rumble() { return false; }
void set_rumble(bool /*on*/) {}

bool camera_swapped() { return true; }
void set_camera_swapped(bool /*swapped*/) {}

} // namespace conker::input
