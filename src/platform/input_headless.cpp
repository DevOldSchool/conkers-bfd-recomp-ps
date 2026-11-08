// No input device: the null renderer has no window to take keyboard focus.
#include "platform/input.hpp"
#include "net/lobby_ui.hpp"

namespace conker::input {

void read(uint16_t* buttons, float* x, float* y, int /*port*/) {
    *buttons = 0;
    *x = 0.0f;
    *y = 0.0f;
}

bool right_stick(float* x, float* y, int /*port*/) {
    *x = 0.0f;
    *y = 0.0f;
    return false;
}

void set_mouse_play(bool /*playing*/) {}
void set_blocked(bool /*blocked*/) {}
bool mouse_captured() { return false; }
void take_mouse(float* dx, float* dy, int* zoom) {
    *dx = 0.0f;
    *dy = 0.0f;
    *zoom = 0;
}

bool connected(int port) { return port == 0; }

std::string controller_name(int /*port*/) { return ""; }

std::string controller_model() { return ""; }

PadKind controller_kind() { return PadKind::None; }
PadKind controller_kind(int /*port*/) { return PadKind::None; }

Controls mapping(int /*port*/, int /*button*/) { return 0; }
void set_mapping(int /*port*/, int /*button*/, Controls /*controls*/) {}
Controls default_mapping(int /*button*/) { return 0; }
void reset_mapping(int /*port*/) {}
bool mapping_is_default(int /*port*/) { return true; }
std::string control_names(Controls /*controls*/, int /*port*/) { return "NONE"; }
PadControl shown_control(int /*port*/, int /*button*/, PadKind /*kind*/) { return PadControl::None; }
void start_listening(int /*port*/) {}
void stop_listening() {}
Controls take_heard() { return 0; }

void read_menu(uint16_t* buttons, float* x, float* y) { read(buttons, x, y, 0); }
void read_menu_pad(uint16_t* buttons, float* x, float* y) { read(buttons, x, y, 0); }

const char* key_row_label(int /*row*/) { return ""; }
std::vector<int> key_mapping(int /*button*/) { return {}; }
std::vector<int> default_key_mapping(int /*button*/) { return {}; }
void set_key_mapping(int /*button*/, const std::vector<int>& /*keys*/) {}
void reset_key_mapping() {}
bool key_mapping_is_default() { return true; }
int shown_key(int /*button*/) { return 0; }
std::string key_label(int /*scancode*/) { return ""; }
std::string key_names(const std::vector<int>& /*keys*/) { return "NONE"; }
void start_listening_keys() {}
int take_heard_key() { return 0; }

bool take_debug_menu_press() { return false; }

bool has_rumble(int /*port*/) { return false; }
void set_rumble(int /*port*/, bool /*on*/) {}
bool rumble_on(int /*port*/) { return false; }
void set_hold_rumble(int /*port*/, bool /*on*/, const RumbleFeel& /*feel*/) {}
void set_rumble_feel(int /*port*/, const RumbleFeel& /*feel*/) {}

bool has_haptics(int /*port*/) { return false; }
void play_haptic(const Haptic& /*haptic*/, int /*port*/) {}

bool has_adaptive_triggers(int /*port*/) { return false; }
void set_trigger_effect(const TriggerEffect& /*effect*/, int /*port*/) {}

bool camera_swapped() { return true; }
void set_camera_swapped(bool /*swapped*/) {}

} // namespace conker::input

// No lobby window without a renderer (src/net/lobby_ui.hpp).
namespace conker::lobby_ui {
void open() {}
void close() {}
bool is_open() { return false; }
void frame(uint8_t* /*rdram*/, recomp_context* /*ctx*/) {}
bool hides_sprite(uint32_t /*resource*/) { return false; }
bool on_barrels(uint8_t* /*rdram*/) { return false; }
}  // namespace conker::lobby_ui
