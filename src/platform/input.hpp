// Player input for the four controller ports. The RT64 build reads SDL game controllers, one per
// port in the order they connect, and the keyboard on controller 1 (src/platform/input_sdl.cpp,
// pumped by the window's event loop); the null build has no input device
// (src/platform/input_headless.cpp). Every port's pad has its own rumble pak, haptics and adaptive
// triggers below; controller 1 alone has the keyboard, mouse, button pictures and debug menu button.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace conker::input {

// N64 button bits (OSContPad.button).
constexpr uint16_t kA = 0x8000;
constexpr uint16_t kB = 0x4000;
constexpr uint16_t kZ = 0x2000;
constexpr uint16_t kStart = 0x1000;
constexpr uint16_t kDUp = 0x0800;
constexpr uint16_t kDDown = 0x0400;
constexpr uint16_t kDLeft = 0x0200;
constexpr uint16_t kDRight = 0x0100;
constexpr uint16_t kL = 0x0020;
constexpr uint16_t kR = 0x0010;
constexpr uint16_t kCUp = 0x0008;
constexpr uint16_t kCDown = 0x0004;
constexpr uint16_t kCLeft = 0x0002;
constexpr uint16_t kCRight = 0x0001;
// Not an N64 button (the pad's bit 0x0040 is unused): Triangle/Y, which the game ignores; in a
// match consistent_controls makes it C-down, the grenade, the right stick being taken there.
constexpr uint16_t kExtra = 0x0040;

constexpr int kPorts = 4;

// Current state of a controller port (0 is controller 1), safe to call from any thread. The stick
// is in [-1, 1] with +y up; ultramodern scales it to the N64 range (about +-82 on the axes).
void read(uint16_t* buttons, float* x, float* y, int port = 0);

// A port's right stick, in [-1, 1] with +y up and the left stick's deadzone, which the game sees
// only as the C buttons (src/enhancements/consistent_controls aims with it). False, with the stick
// centred, when no pad reads the port (controller 1 on the keyboard alone). Safe to call from
// any thread.
bool right_stick(float* x, float* y, int port = 0);

// Controller 1's mouse (and the R and T keys' zoom). The game thread says each frame whether the
// game is being played, not paused or in a menu, which is when the mouse may be captured; take_mouse
// gives the motion (pixels, +x right, +y down) and zoom steps (+ in, - out) since the last call,
// and clears them. Safe to call from any thread.
void set_mouse_play(bool playing);

// While a window over the game takes the input (the ONLINE lobby, src/net/lobby_ui_rt64.cpp):
// the game reads every port as nothing pressed, no mouse motion and no debug menu press, and the
// mouse is not captured; once it lets go, each port reads nothing until its buttons are let go (the
// press that closed the window is not the game's). Safe to call from any thread.
void set_blocked(bool blocked);
bool mouse_captured();
void take_mouse(float* dx, float* dy, int* zoom);

// Whether a controller is plugged into a port: controller 1 always is (the keyboard), the others
// while a pad holds them. Safe to call from any thread.
bool connected(int port);

// A port's pad's name, or "" when none is connected (controller 1 on the keyboard only). Safe to
// call from any thread.
std::string controller_name(int port = 0);

// Controller 1's make and model in general terms ("DualSense", "Xbox", "Controller"), or "" when none
// is connected: what other players, logs and reports get. The name is often the owner's (macOS names a
// Bluetooth pad after its owner), so it is shown on this machine only. Safe to call from any thread.
std::string controller_model();

// The kind of pad controller 1 is, for its button pictures (src/enhancements/pad_icons): None for
// the keyboard alone, Other for pads that are neither Xbox nor PlayStation ones. Safe to call from
// any thread.
enum class PadKind { None, Xbox, PlayStation, Other };
PadKind controller_kind();
PadKind controller_kind(int port);  // any port's

// True once for each press of the debug menu button: the pad's Back (Create/Share on a
// PlayStation pad, View on an Xbox pad) or the ` key. Safe to call from any thread.
bool take_debug_menu_press();

// A port's rumble pak: present while that port's pad can rumble. set_rumble turns the pak's motor
// on or off; both are safe to call from any thread.
bool has_rumble(int port = 0);
void set_rumble(int port, bool on);

// A haptic effect on a port's pad, alongside the rumble pak's on and off: shaped on a DualSense on
// macOS (Core Haptics), played on the rumble motors (and an Xbox pad's trigger motors) elsewhere.
// intensity and sharpness are in [0, 1] (sharpness 0 is a soft thump, 1 a crisp tap); duration 0
// is a single tap, longer a steady buzz. Grip::Trigger is the side of the trigger last pulled for Z
// on that pad (right until one is). has_haptics says whether the port's pad plays them; both are
// safe to call from any thread.
struct Haptic {
    enum class Grip { Left, Right, Both, Trigger };
    float intensity = 1.0f;
    float sharpness = 0.5f;
    float duration = 0.0f;  // seconds
    Grip grip = Grip::Both;
};
bool has_haptics(int port = 0);
void play_haptic(const Haptic& haptic, int port = 0);

// What a port's triggers (both are Z) do on a pad with adaptive triggers (a DualSense).
// Positions along the pull and strengths are in [0, 1], from released to fully pressed:
// - Off: the triggers are free.
// - Feedback: zones[i] resists in each tenth of the pull.
// - Weapon: free up to start, then a wall of strength up to end, where it gives way with a snap.
// - Vibration: buzzes past start at amplitude and frequency (0 slowest, 1 fastest).
// z_at is how far a trigger is pulled before it counts as Z on such a pad, so an effect can lead up
// to it; other pads keep kTriggerZ. has_adaptive_triggers says whether the port's pad has them;
// both are safe to call from any thread.
constexpr int kTriggerZones = 10;
constexpr float kTriggerZ = 10000.0f / 32767.0f;  // about a third of the travel
struct TriggerEffect {
    enum class Mode { Off, Feedback, Weapon, Vibration };
    Mode mode = Mode::Off;
    float zones[kTriggerZones] = {};
    float start = 0.0f;
    float end = 0.0f;
    float strength = 0.0f;
    float amplitude = 0.0f;
    float frequency = 0.0f;
    float z_at = kTriggerZ;
    bool operator==(const TriggerEffect&) const = default;
};
bool has_adaptive_triggers(int port = 0);
void set_trigger_effect(const TriggerEffect& effect, int port = 0);

// --- the button mapping (the debug menu's CONTROLS page) ---
// Which of a port's pad's controls press each of the game's buttons, kept per port in
// conker-recomp-data/input.cfg (src/platform/input_config.hpp). A control is one of the pad's
// buttons, a trigger or the right stick pushed one way; Controls has a bit for each. The pad's Back
// (the debug menu's) and its guide button are never mapped, and the keyboard keeps its keys.
// The mapped buttons go into the frame records, so replays and online play see what was pressed.
// Safe to call from any thread.
using Controls = uint32_t;

// The game's buttons the mapping has, in the CONTROLS page's order. C-LEFT and C-RIGHT are what the
// right stick pushed left and right gives: the view turning left and right (C-right and C-left on
// the N64, swapped by camera_swapped below). kExtra, Triangle, is only the grenade in a match.
struct MappedButton {
    uint16_t n64;
    const char* label;  // for the game's font
    const char* key;    // in input.cfg
};
constexpr MappedButton kMappedButtons[] = {
    {kA, "A", "a"},
    {kB, "B", "b"},
    {kZ, "Z", "z"},
    {kStart, "START", "start"},
    {kL, "L", "l"},
    {kR, "R", "r"},
    {kDUp, "D-PAD UP", "dup"},
    {kDDown, "D-PAD DOWN", "ddown"},
    {kDLeft, "D-PAD LEFT", "dleft"},
    {kDRight, "D-PAD RIGHT", "dright"},
    {kCUp, "C-UP", "cup"},
    {kCDown, "C-DOWN", "cdown"},
    {kCRight, "C-LEFT", "cleft"},
    {kCLeft, "C-RIGHT", "cright"},
    {kExtra, "GRENADE IN MULTI", "extra"},
};
constexpr int kMappedButtonCount = static_cast<int>(sizeof(kMappedButtons) / sizeof(kMappedButtons[0]));

// The controls that press kMappedButtons[button] on a port's pad.
Controls mapping(int port, int button);
// Sets them, taking them off the port's other buttons, and keeps the port's mapping.
void set_mapping(int port, int button, Controls controls);
// The controls that press it by default (the header of src/platform/input_sdl.cpp).
Controls default_mapping(int button);
// Every button back to its default.
void reset_mapping(int port);
bool mapping_is_default(int port);
// The controls' names on the port's kind of pad ("CROSS / SQUARE"), or "NONE".
std::string control_names(Controls controls, int port);

// A pad's control, as the button pictures name it (src/enhancements/pad_icons): SDL's buttons in
// SDL's order (by position, Xbox names: South is A and Cross), then the triggers and the right
// stick's four ways.
enum class PadControl {
    None, South, East, West, North, Back, Guide, Start, LeftStick, RightStick, LeftShoulder, RightShoulder,
    DpadUp, DpadDown, DpadLeft, DpadRight, Misc, Paddle1, Paddle2, Paddle3, Paddle4, Touchpad,
    LeftTrigger, RightTrigger, RightStickUp, RightStickDown, RightStickLeft, RightStickRight,
};

// The control a picture of kMappedButtons[button] shows on a `kind` pad: the one the default
// pictures show (B as Xbox B or PlayStation Square, Z as the right trigger) while it still presses
// the button, else the first that does; None when none does.
PadControl shown_control(int port, int button, PadKind kind);

// Listens for the next control pressed on the port's pad (one held as it starts counts once let go
// and pressed again); take_heard gives it once, as a single bit, or 0 while none has been. A new
// start, or stop_listening, forgets what was heard.
void start_listening(int port);
void stop_listening();
Controls take_heard();

// The keyboard's rows: the jobs only keys have, after kMappedButtons on the CONTROLS page. The
// stick is pushed fully by its keys, half way with WALK held; ZOOM IN and OUT move the camera
// through its distances, as the mouse wheel does.
struct KeyAction {
    const char* label;  // for the game's font
    const char* key;    // in input.cfg
};
constexpr KeyAction kKeyActions[] = {
    {"STICK UP", "stickup"}, {"STICK DOWN", "stickdown"}, {"STICK LEFT", "stickleft"}, {"STICK RIGHT", "stickright"},
    {"WALK", "walk"}, {"ZOOM IN", "zoomin"}, {"ZOOM OUT", "zoomout"},
};
constexpr int kKeyActionCount = static_cast<int>(sizeof(kKeyActions) / sizeof(kKeyActions[0]));
constexpr int kKeyRows = kMappedButtonCount + kKeyActionCount;
// A keyboard row's label: kMappedButtons' then kKeyActions'.
const char* key_row_label(int row);

// The keyboard's keys for each of its rows (controller 1's): kMappedButtons, then kKeyActions,
// as SDL scancodes, kept in input.cfg too ("keys=<row>:<scancode>,...;..."). ` (the debug menu),
// Esc (lets the mouse go), Cmd and Alt (the system's shortcuts) are never mapped, nor the mouse's
// buttons (Z and R).
std::vector<int> key_mapping(int row);
std::vector<int> default_key_mapping(int row);
// Sets a row's keys, taking them off the other rows, and keeps the mapping.
void set_key_mapping(int row, const std::vector<int>& keys);
void reset_key_mapping();
bool key_mapping_is_default();
// The key a picture of kMappedButtons[button] shows: the one the default pictures show while it
// still presses the button, else the first that does; 0 when none does.
int shown_key(int button);
// A key's name for the game's font ("SPACE", "LCTRL", "F1", "UP"), short enough for a keycap, and
// keys' names joined ("SPACE / C") or "NONE".
std::string key_label(int scancode);
std::string key_names(const std::vector<int>& keys);
// As start_listening, for the next key pressed (one that can be mapped); take_heard_key gives it
// once, or 0. stop_listening stops both.
void start_listening_keys();
int take_heard_key();

// Controller 1 as the default mapping has its pad, with the keyboard: what the debug menu reads, so
// it can be steered whatever the mapping is.
void read_menu(uint16_t* buttons, float* x, float* y);

// Whether C-left and C-right are swapped, on the right stick and on J/L, so that pushing left turns
// the view left; part of consistent controls (src/enhancements/consistent_controls), which set it.
// Safe to call from any thread.
bool camera_swapped();
void set_camera_swapped(bool swapped);

} // namespace conker::input
