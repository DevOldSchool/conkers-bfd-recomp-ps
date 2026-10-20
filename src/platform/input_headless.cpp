// No input device: the null renderer has no window to take keyboard focus.
#include "platform/input.hpp"

#include <atomic>

namespace conker::input {

namespace {
std::atomic<uint16_t> g_forced_buttons{0};
}  // namespace

void set_forced_buttons(uint16_t buttons) { g_forced_buttons.store(buttons, std::memory_order_relaxed); }

void read(uint16_t* buttons, float* x, float* y) {
    *buttons = g_forced_buttons.load(std::memory_order_relaxed);
    *x = 0.0f;
    *y = 0.0f;
}

bool take_debug_menu_press() { return false; }

bool has_rumble() { return false; }
void set_rumble(bool /*on*/) {}

} // namespace conker::input
