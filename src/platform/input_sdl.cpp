// The four controller ports from SDL game controllers, and controller 1 from the keyboard too.
// update() samples them on the main thread after the window pumps events; the game thread reads
// the result through read() when the game polls the controllers. A pad takes the lowest free port
// when it connects and frees it when it disconnects; the others keep theirs. Controller 1 combines
// its pad and the keyboard: buttons are OR-ed, and the pad's stick wins over the keyboard's
// whenever it is outside the deadzone.
//
// Pad (SDL's Xbox-style names), by default: south A, west and east B, both triggers Z, LB L, RB R,
// Start Start, D-pad D-pad, Y the grenade in a match, left stick the analog stick, right stick the
// C buttons (kDefaultMapping). The debug menu's CONTROLS page maps the buttons differently per port
// (input.hpp's mapping; kept in input.cfg as "map<port>=<button>:<control>,...;..."). Keyboard and
// mouse: see kKeys, the mouse below and docs/enhancements.md.
// Back (Create/Share on a PlayStation pad, View on an Xbox pad) and the ` key open the debug menu
// (src/enhancements/debug_menu); they are not N64 buttons.
//
// A pad that can rumble (SDL_GameControllerHasRumble; a DualSense, DualShock 4, Xbox or Switch
// Pro pad) counts as a rumble pak in its port, and the game's motor on/off drives both of its
// motors at full strength, like the pak's single motor. Each port's pad has its own haptic effects,
// Core Haptics and adaptive triggers below. Rumble on Bluetooth PlayStation pads needs
// SDL's extended reports, which init() turns on; on macOS those pads go through Apple's
// GameController framework instead, which rumbles without them. There a DualSense plays the pak
// through Core Haptics as a deep rumble in both grips (src/platform/haptics_apple.mm), since SDL's
// rumble on its voice coils is a thin buzz.
//
// play_haptic's shaped effects play on every pad that can rumble. On macOS a DualSense plays them
// through Core Haptics in src/platform/haptics_apple.mm. Other pads, and every pad on Windows and
// Linux, play them on their rumble motors (motor_effect): the heavy low-frequency motor in the left
// grip, the light high-frequency one in the right, and on pads with motors in the triggers (Xbox One
// and Series pads, where SDL can reach them) a Grip::Trigger effect plays in that trigger instead.
// CONKER_HAPTICS_MOTORS=1 plays them on the motors on a DualSense on macOS and on the virtual pad too.
//
// A DualSense's adaptive triggers act as set_trigger_effect asks for its port, both triggers alike since
// both are Z. SDL sends the effect where its own driver reads the pad (HIDAPI); on macOS, where
// Apple's framework reads it, src/platform/adaptive_triggers_apple.mm sets them instead.
//
// CONKER_VIRTUAL_PAD=<file>[,<file>...] attaches an SDL virtual game controller per file, driven by
// its script, so the whole pad path (hot-plug, ports, mapping, deadzone) can be exercised without
// hardware. Each line is "<vi> <length in VIs> <control>...", where a control is an SDL button
// name (a, b, x, start, leftshoulder, dpup, ...) or "<axis>=<value>" (leftx, lefty, rightx, righty:
// -1 to 1, SDL's +y is down; lefttrigger, righttrigger: 0 released to 1), or "plug <vi>" /
// "unplug <vi>". '#' starts a comment. Real pads are then ignored. The first pad's script can also
// play controller 1's mouse: mousex=<pixels> and mousey=<pixels> move it that much every VI,
// wheel=<steps> turns the wheel once (+ away from you), lmb and rmb hold its buttons; a scripted
// run's mouse counts as captured whenever the game is being played.
// The virtual pad can rumble and logs each change, logs each haptic effect (or, with
// CONKER_HAPTICS_MOTORS=1, the motor strengths it plays as), and has adaptive triggers, logging
// each effect sent to them; each line names the virtual pad, and its port when they differ.
#include "platform/input.hpp"
#include "platform/input_sdl.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "platform/input_config.hpp"
#include "runtime/game.hpp"

#ifdef __APPLE__
#include "platform/haptics_apple.hpp"
#include "platform/adaptive_triggers_apple.hpp"
#endif

namespace conker::input {

namespace {

constexpr float kStickDeadzone = 0.20f;        // radial, of full deflection
constexpr Sint16 kCButtonThreshold = 16384;    // right stick, half deflection
constexpr Uint16 kRumbleStrength = 0xFFFF;     // both motors; the pak's motor is on or off
constexpr Uint32 kRumbleHoldMs = 250;          // renewed every update, so a stalled game stops it

// A pad's controls (input.hpp's Controls): SDL's buttons by their number, then the triggers and the
// right stick's four ways.
constexpr int kLeftTrigger = SDL_CONTROLLER_BUTTON_MAX;
constexpr int kRightTrigger = kLeftTrigger + 1;
constexpr int kRightStickUp = kLeftTrigger + 2;
constexpr int kRightStickDown = kLeftTrigger + 3;
constexpr int kRightStickLeft = kLeftTrigger + 4;
constexpr int kRightStickRight = kLeftTrigger + 5;
constexpr int kControlCount = kLeftTrigger + 6;
static_assert(kControlCount <= 32, "a control per bit of Controls");

constexpr Controls bit(int control) { return Controls{1} << control; }

// Never mapped: Back opens the debug menu, the guide button is the system's.
constexpr Controls kUnmappable = bit(SDL_CONTROLLER_BUTTON_BACK) | bit(SDL_CONTROLLER_BUTTON_GUIDE);

// The controls for each of kMappedButtons, by default.
constexpr Controls kDefaultMapping[kMappedButtonCount] = {
    bit(SDL_CONTROLLER_BUTTON_A),                                  // A
    bit(SDL_CONTROLLER_BUTTON_X) | bit(SDL_CONTROLLER_BUTTON_B),   // B
    // Z sits under the right hand on modern pads (R2/RT); the left trigger stays Z as well.
    bit(kLeftTrigger) | bit(kRightTrigger),                        // Z
    bit(SDL_CONTROLLER_BUTTON_START),                              // Start
    bit(SDL_CONTROLLER_BUTTON_LEFTSHOULDER),                       // L
    bit(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER),                      // R
    bit(SDL_CONTROLLER_BUTTON_DPAD_UP),                            // D-pad
    bit(SDL_CONTROLLER_BUTTON_DPAD_DOWN),
    bit(SDL_CONTROLLER_BUTTON_DPAD_LEFT),
    bit(SDL_CONTROLLER_BUTTON_DPAD_RIGHT),
    bit(kRightStickUp),                                            // C-up
    bit(kRightStickDown),                                          // C-down
    bit(kRightStickLeft),                                          // C-left: the view turns left
    bit(kRightStickRight),                                         // C-right
    bit(SDL_CONTROLLER_BUTTON_Y),  // Triangle: C-down in a match (consistent_controls)
};

std::atomic<Controls> g_mapping[kPorts][kMappedButtonCount];  // set by load_mapping()

struct Key {
    SDL_Scancode key;
    uint16_t n64;
};

// WASD is the stick (Shift held walks, at half tilt), the mouse the camera (see the mouse below),
// and R and T zoom it in and out as the wheel does (take_mouse), not N64 buttons. These are the
// default keys: the debug menu's CONTROLS page maps them differently (key_mapping), and the menu
// itself reads these.
constexpr Key kKeys[] = {
    {SDL_SCANCODE_SPACE, kA},
    {SDL_SCANCODE_E, kB},
    {SDL_SCANCODE_LCTRL, kZ},  // crouch; the left mouse button is Z too (fire)
    {SDL_SCANCODE_C, kZ},
    {SDL_SCANCODE_RETURN, kStart},
    {SDL_SCANCODE_KP_ENTER, kStart},
    {SDL_SCANCODE_Q, kL},
    {SDL_SCANCODE_TAB, kL},
    {SDL_SCANCODE_F, kR},  // the right mouse button is R too (aim)
    {SDL_SCANCODE_I, kCUp},
    {SDL_SCANCODE_K, kCDown},
    {SDL_SCANCODE_J, kCRight},  // swapped like the right stick: J turns the view left
    {SDL_SCANCODE_L, kCLeft},
    {SDL_SCANCODE_UP, kDUp},
    {SDL_SCANCODE_DOWN, kDDown},
    {SDL_SCANCODE_LEFT, kDLeft},
    {SDL_SCANCODE_RIGHT, kDRight},
};
constexpr float kWalkTilt = 0.5f;  // the stick with Shift held

// The keys for kKeyActions by default: W A S D the stick, Shift walking, R and T the zoom.
const std::vector<int> kKeyActionDefaults[kKeyActionCount] = {
    {SDL_SCANCODE_W}, {SDL_SCANCODE_S}, {SDL_SCANCODE_A}, {SDL_SCANCODE_D},
    {SDL_SCANCODE_LSHIFT, SDL_SCANCODE_RSHIFT}, {SDL_SCANCODE_R}, {SDL_SCANCODE_T},
};
enum KeyActionRow { kStickUp = kMappedButtonCount, kStickDown, kStickLeft, kStickRight, kWalk, kZoomIn, kZoomOut };

// Keys never mapped: the debug menu, letting the mouse go, and the system's shortcuts.
constexpr SDL_Scancode kUnmappableKeys[] = {
    SDL_SCANCODE_GRAVE, SDL_SCANCODE_ESCAPE, SDL_SCANCODE_LGUI, SDL_SCANCODE_RGUI, SDL_SCANCODE_LALT, SDL_SCANCODE_RALT,
};

bool key_mappable(int key) {
    return key > SDL_SCANCODE_UNKNOWN && key < SDL_NUM_SCANCODES &&
           std::find(std::begin(kUnmappableKeys), std::end(kUnmappableKeys), key) == std::end(kUnmappableKeys);
}

// Controller 1's keys for each keyboard row (key_mapping), set by load_mapping().
std::mutex g_key_mutex;
std::vector<int> g_key_mapping[kKeyRows];  // under g_key_mutex

const char* row_key(int row) { return row < kMappedButtonCount ? kMappedButtons[row].key : kKeyActions[row - kMappedButtonCount].key; }

// The mouse, for controller 1. Clicking in the window during play captures it (relative mode, no
// cursor; that click is not passed on); the game thread says whether it is playing
// (set_mouse_play). Capture lets go while the game is paused or in a menu, and comes back by itself
// when play carries on; losing the window's focus, or Esc, lets go until the next click. Esc is
// never Start (Enter is, as the keyboard's button pictures show). While captured the left button
// is Z (fire), the right R (aim), its motion goes to take_mouse for the camera and aiming, and the
// wheel zooms.
bool g_scripted = false;  // CONKER_VIRTUAL_PAD: only the virtual pads count
std::atomic<bool> g_mouse_play{false};     // set by the game thread
std::atomic<bool> g_blocked{false};        // a window over the game (the ONLINE lobby) takes the input
// After it lets the input go, each port reads nothing until its buttons are let go, so the press
// that closed the window (B, say) does not reach the game as a new one.
std::atomic<bool> g_wait_release[kPorts] = {};
bool g_mouse_captured = false;             // main thread only
bool g_mouse_recapture = false;            // main thread only: captured before the game left play
bool g_mouse_swallow = false;              // main thread only: the capturing click is still held
std::mutex g_mouse_mutex;
float g_mouse_dx = 0.0f;                   // under g_mouse_mutex: motion not yet taken, pixels
float g_mouse_dy = 0.0f;
int g_mouse_zoom = 0;                      // under g_mouse_mutex: zoom steps, + in
bool g_zoom_in_held = false;               // main thread only: R and T, for their presses
bool g_zoom_out_held = false;
std::atomic<bool> g_mouse_buttons_captured{false};  // g_mouse_captured, for other threads
bool g_script_mouse_left = false;          // main thread only: a virtual pad script's mouse buttons
bool g_script_mouse_right = false;
std::vector<SDL_Scancode> g_script_keys;   // main thread only: keys a virtual pad script holds

bool window_focused() {
    SDL_Window* window = SDL_GetKeyboardFocus();
    return window != nullptr && (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0;
}

void capture_mouse(bool on) {
    if (on == g_mouse_captured) {
        return;
    }
    if (SDL_SetRelativeMouseMode(on ? SDL_TRUE : SDL_FALSE) != 0) {
        return;
    }
    g_mouse_captured = on;
    g_mouse_buttons_captured.store(on, std::memory_order_relaxed);
    std::lock_guard lock(g_mouse_mutex);
    g_mouse_dx = 0.0f;
    g_mouse_dy = 0.0f;
}

void add_zoom(int steps) {
    std::lock_guard lock(g_mouse_mutex);
    g_mouse_zoom = std::clamp(g_mouse_zoom + steps, -2, 2);
}

void mouse_event(const SDL_Event& event) {
    switch (event.type) {
        case SDL_MOUSEBUTTONDOWN:
            if (!g_mouse_captured && g_mouse_play.load(std::memory_order_relaxed) &&
                !g_blocked.load(std::memory_order_relaxed) && window_focused()) {
                capture_mouse(true);
                g_mouse_swallow = g_mouse_captured;
            }
            break;
        case SDL_MOUSEMOTION:
            if (g_mouse_captured) {
                std::lock_guard lock(g_mouse_mutex);
                g_mouse_dx += static_cast<float>(event.motion.xrel);
                g_mouse_dy += static_cast<float>(event.motion.yrel);
            }
            break;
        case SDL_MOUSEWHEEL:
            if (g_mouse_captured && event.wheel.y != 0) {
                const int y = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event.wheel.y : event.wheel.y;
                add_zoom(y > 0 ? 1 : -1);  // away from you zooms in
            }
            break;
        case SDL_KEYDOWN:
            if (event.key.keysym.scancode == SDL_SCANCODE_ESCAPE && !event.key.repeat) {
                if (g_mouse_captured) {
                    capture_mouse(false);
                    g_mouse_recapture = false;
                }
            }
            break;
        case SDL_WINDOWEVENT:
            if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                capture_mouse(false);
                g_mouse_recapture = false;
            }
            break;
        default:
            break;
    }
}

// Once an update: capture follows play, and the mouse's buttons and R/T join controller 1's.
void update_mouse(uint16_t& buttons, bool zoom_in, bool zoom_out) {
    const bool play = g_mouse_play.load(std::memory_order_relaxed) && !g_blocked.load(std::memory_order_relaxed);
    if (g_scripted) {
        g_mouse_captured = play;  // no real capture, which needs the window's focus
        g_mouse_buttons_captured.store(play, std::memory_order_relaxed);
        if (play && g_script_mouse_left) buttons |= kZ;
        if (play && g_script_mouse_right) buttons |= kR;
        return;
    }
    if (g_mouse_captured && !play) {
        capture_mouse(false);
        g_mouse_recapture = true;
    } else if (!g_mouse_captured && g_mouse_recapture && play && window_focused()) {
        capture_mouse(true);
        g_mouse_recapture = !g_mouse_captured;
    }
    const Uint32 mouse = SDL_GetMouseState(nullptr, nullptr);
    if (g_mouse_swallow && (mouse & (SDL_BUTTON_LMASK | SDL_BUTTON_RMASK)) == 0) {
        g_mouse_swallow = false;
    }
    if (g_mouse_captured && !g_mouse_swallow) {
        if (mouse & SDL_BUTTON_LMASK) buttons |= kZ;
        if (mouse & SDL_BUTTON_RMASK) buttons |= kR;
    }
    if (zoom_in && !g_zoom_in_held) add_zoom(1);
    if (zoom_out && !g_zoom_out_held) add_zoom(-1);
    g_zoom_in_held = zoom_in;
    g_zoom_out_held = zoom_out;
}

std::atomic<int> g_debug_menu_presses{0};
std::atomic<uint16_t> g_menu_buttons{0};  // controller 1 as the default mapping has it (read_menu)
// Controller 1's pad alone, as the default mapping has it, and its stick (read_menu_pad).
std::atomic<uint16_t> g_menu_pad_buttons{0};
std::atomic<float> g_menu_pad_x{0.0f};
std::atomic<float> g_menu_pad_y{0.0f};
std::mutex g_pad_name_mutex;             // the ports' names
bool g_debug_menu_held = false;          // main thread only

std::atomic<PadKind> g_pad_kind{PadKind::None};
std::atomic<bool> g_camera_swapped{true};  // C-left and C-right (camera_swapped)

// How a pad plays haptic effects.
enum class HapticsVia { CoreHaptics, Motors, Log };

// An effect playing on the rumble motors, each strength in [0, 1], until end_ms (SDL_GetTicks64).
struct MotorEffect {
    Uint64 end_ms;
    float low;
    float high;
    float left_trigger;
    float right_trigger;
};
constexpr float kSharpTap = 0.04f;         // s: a tap on the motors at sharpness 1, which spin up fast
constexpr float kSoftTap = 0.09f;          // s: at sharpness 0, a thump the heavy motor needs longer for

const bool g_force_motors = [] {
    const char* value = std::getenv("CONKER_HAPTICS_MOTORS");
    return value != nullptr && value[0] == '1';
}();
std::mutex g_haptics_mutex;              // the ports' waiting haptics
constexpr size_t kMaxWaitingHaptics = 8;

std::mutex g_trigger_mutex;              // the ports' trigger_wanted

// A controller port and the pad holding it, with the pad's rumble, haptics and adaptive triggers.
// Controller 1 also plays the keyboard alongside its pad, and has a port without a pad.
struct Port {
    SDL_GameController* pad = nullptr;  // main thread only
    SDL_JoystickID id = -1;             // main thread only
    bool rumble_applied = false;        // main thread only
    std::atomic<bool> connected{false};
    std::atomic<uint16_t> buttons{0};
    std::atomic<float> stick_x{0.0f};
    std::atomic<float> stick_y{0.0f};
    std::atomic<float> right_x{0.0f};  // the right stick, for right_stick()
    std::atomic<float> right_y{0.0f};
    std::atomic<bool> has_pad{false};  // a pad is reading the port, not the keyboard alone
    std::atomic<PadKind> kind{PadKind::None};  // for its controls' names
    std::atomic<bool> has_rumble{false};  // the pad can rumble
    std::atomic<bool> rumble_on{false};   // the game's motor state, applied by update()
    std::string name;                     // under g_pad_name_mutex: the pad's, for controller_name()
    std::string model;                    // under g_pad_name_mutex: its make and model, for controller_model()
    int virtual_number = 0;               // main thread only: its CONKER_VIRTUAL_PAD number, or 0

    // Haptic effects (the rest main thread only).
    std::atomic<bool> has_haptics{false};  // the pad plays shaped effects
    std::vector<Haptic> haptics;           // waiting for update(), under g_haptics_mutex
    HapticsVia haptics_via = HapticsVia::Motors;
    std::vector<MotorEffect> motor_effects;
    bool has_trigger_rumble = false;       // SDL_GameControllerHasRumbleTriggers
    bool trigger_rumble_applied = false;
    bool pak_rumble_applied = false;       // the pak's rumble playing through Core Haptics
    Haptic::Grip trigger_grip = Haptic::Grip::Right;  // Grip::Trigger's side
    bool left_trigger_held = false;
    bool right_trigger_held = false;
    bool pad_virtual = false;              // the pad is CONKER_VIRTUAL_PAD's

    // Adaptive triggers (the rest main thread only).
    std::atomic<bool> adaptive_triggers{false};  // the pad has them
    TriggerEffect trigger_wanted;                // set_trigger_effect's, under g_trigger_mutex
    TriggerEffect trigger_applied;
    bool trigger_stale = true;                   // the pad has not had trigger_applied

    int number() const;
};
Port g_ports[kPorts];
Port& g_port1 = g_ports[0];

int Port::number() const { return static_cast<int>(this - g_ports) + 1; }

PadKind pad_kind(SDL_GameController* pad) {
    switch (SDL_GameControllerGetType(pad)) {
        case SDL_CONTROLLER_TYPE_XBOX360:
        case SDL_CONTROLLER_TYPE_XBOXONE:
            return PadKind::Xbox;
        case SDL_CONTROLLER_TYPE_PS3:
        case SDL_CONTROLLER_TYPE_PS4:
        case SDL_CONTROLLER_TYPE_PS5:
            return PadKind::PlayStation;
        default:
            return PadKind::Other;
    }
}

// A pad's make and model in general terms, for other players and for logs and reports: never its name,
// which macOS takes from the pad's Bluetooth name, and that is often its owner's ("<Name>'s DualSense").
// From SDL's type, or for a pad SDL does not know (a virtual one too), from its USB vendor and product.
std::string pad_model(SDL_GameController* pad) {
    switch (SDL_GameControllerGetType(pad)) {
        case SDL_CONTROLLER_TYPE_PS5:
            return "DualSense";
        case SDL_CONTROLLER_TYPE_PS4:
            return "DualShock 4";
        case SDL_CONTROLLER_TYPE_PS3:
            return "DualShock 3";
        case SDL_CONTROLLER_TYPE_XBOX360:
        case SDL_CONTROLLER_TYPE_XBOXONE:
            return "Xbox";
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO:
            return "Switch Pro";
        default:
            break;
    }
    const uint16_t product = SDL_GameControllerGetProduct(pad);
    switch (SDL_GameControllerGetVendor(pad)) {
        case 0x054C:
            return product == 0x0CE6 || product == 0x0DF2 ? "DualSense"
                   : product == 0x05C4 || product == 0x09CC ? "DualShock 4"
                                                            : "PlayStation";
        case 0x045E:
            return "Xbox";
        case 0x057E:
            return product == 0x2009 ? "Switch Pro" : "Nintendo";
        default:
            return "Controller";
    }
}

// CONKER_RUMBLE_TEST=1: one second of rumble on connecting, to check the pad without the game.
void rumble_test(const Port& port) {
    const char* test = std::getenv("CONKER_RUMBLE_TEST");
    if (port.has_rumble.load() && test != nullptr && test[0] == '1') {
        const int result = SDL_GameControllerRumble(port.pad, kRumbleStrength, kRumbleStrength, 1000);
        std::printf("[conker] rumble test: %s\n", result == 0 ? "1 s at full strength" : SDL_GetError());
    }
}

int virtual_pad_number(SDL_JoystickID id);

// A port's new pad: its name, haptics and adaptive triggers, and for controller 1 its picture kind.
void open_pad_extras(Port& port, int device_index) {
    {
        std::lock_guard lock(g_pad_name_mutex);
        port.name = SDL_GameControllerName(port.pad) ? SDL_GameControllerName(port.pad) : "controller";
        port.model = pad_model(port.pad);
    }
    port.kind.store(pad_kind(port.pad));
    if (&port == &g_port1) {
        g_pad_kind.store(pad_kind(port.pad));
    }
#ifdef __APPLE__
    if (port.pak_rumble_applied) {
        apple::set_dualsense_rumble(port.number() - 1, false);
    }
#endif
    port.pak_rumble_applied = false;
    port.pad_virtual = SDL_JoystickIsVirtual(device_index) == SDL_TRUE;
    port.virtual_number = port.pad_virtual ? virtual_pad_number(port.id) : 0;
    port.has_trigger_rumble = SDL_GameControllerHasRumbleTriggers(port.pad) == SDL_TRUE;
    port.trigger_rumble_applied = false;
    port.motor_effects.clear();
    port.trigger_grip = Haptic::Grip::Right;
    port.left_trigger_held = port.right_trigger_held = false;
    port.haptics_via = HapticsVia::Motors;
#ifdef __APPLE__
    if (SDL_GameControllerGetType(port.pad) == SDL_CONTROLLER_TYPE_PS5 && !g_force_motors) {
        port.haptics_via = HapticsVia::CoreHaptics;
    }
#endif
    if (port.pad_virtual && !g_force_motors) {
        port.haptics_via = HapticsVia::Log;
    }
    port.has_haptics.store(port.has_rumble.load() || port.pad_virtual);
    static const char* const kVia[] = {"Core Haptics", "motors", "logged"};
    port.adaptive_triggers.store(SDL_GameControllerGetType(port.pad) == SDL_CONTROLLER_TYPE_PS5 || port.pad_virtual);
    port.trigger_stale = true;
    const std::string which = port.virtual_number != 0 ? " (virtual pad " + std::to_string(port.virtual_number) + ")" : "";
    std::printf("[conker] controller %d: %s%s (%s%s%s%s%s)\n", port.number(), pad_model(port.pad).c_str(),
                which.c_str(), port.has_rumble.load() ? "rumble pak" : "no rumble",
                port.has_haptics.load() ? ", haptics on " : "",
                port.has_haptics.load() ? kVia[static_cast<int>(port.haptics_via)] : "",
                port.has_trigger_rumble ? ", trigger motors" : "", port.adaptive_triggers.load() ? ", adaptive triggers" : "");
}

void close_pad_extras(Port& port) {
#ifdef __APPLE__
    if (port.pak_rumble_applied) {
        apple::set_dualsense_rumble(port.number() - 1, false);
    }
#endif
    port.pak_rumble_applied = false;
    {
        std::lock_guard lock(g_pad_name_mutex);
        port.name.clear();
        port.model.clear();
    }
    {
        std::lock_guard lock(g_haptics_mutex);
        port.haptics.clear();
    }
    port.motor_effects.clear();
    port.has_haptics.store(false);
    port.adaptive_triggers.store(false);
    port.kind.store(PadKind::None);
    if (&port == &g_port1) {
        g_pad_kind.store(PadKind::None);
    }
}

// Gives the pad the lowest free port, unless it already has one or all four are taken.
void open_pad(int device_index) {
    if (!SDL_IsGameController(device_index)) {
        return;
    }
    // A real pad would take a port from the script, and SDL hands pad input to every process, so a
    // scripted run would also pick up whatever is played in another window.
    if (g_scripted && !SDL_JoystickIsVirtual(device_index)) {
        return;
    }
    const SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(device_index);
    Port* free_port = nullptr;
    for (Port& port : g_ports) {
        if (port.pad != nullptr && port.id == id) {
            return;
        }
        if (port.pad == nullptr && free_port == nullptr) {
            free_port = &port;
        }
    }
    if (free_port == nullptr) {
        return;
    }
    Port& port = *free_port;
    const int number = static_cast<int>(&port - g_ports) + 1;
    port.pad = SDL_GameControllerOpen(device_index);
    if (port.pad == nullptr) {
        std::fprintf(stderr, "[conker] could not open controller %d: %s\n", device_index, SDL_GetError());
        return;
    }
    port.id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(port.pad));
    // The port as the pad's player number: lit on a DualSense, and how src/platform/haptics_apple.mm
    // and adaptive_triggers_apple.mm find each port's pad among the GameController framework's.
    SDL_GameControllerSetPlayerIndex(port.pad, number - 1);
    port.has_rumble.store(SDL_GameControllerHasRumble(port.pad) == SDL_TRUE);
    port.rumble_applied = false;
    port.buttons.store(0);
    port.stick_x.store(0.0f);
    port.stick_y.store(0.0f);
    open_pad_extras(port, device_index);
    port.connected.store(true);
    rumble_test(port);
}

void open_all_pads() {
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        open_pad(i);
    }
}

void close_pad(Port& port) {
    std::printf("[conker] controller %d disconnected\n", port.number());
    close_pad_extras(port);
    port.connected.store(false);
    port.has_rumble.store(false);
    port.buttons.store(0);
    port.stick_x.store(0.0f);
    port.stick_y.store(0.0f);
    SDL_GameControllerClose(port.pad);
    port.pad = nullptr;
    port.id = -1;
}

// --- scripted virtual pad (CONKER_VIRTUAL_PAD) ---
struct ScriptStep {
    uint64_t vi;
    uint64_t length;
    std::string text;
    std::vector<SDL_GameControllerButton> buttons;
    std::vector<std::pair<SDL_GameControllerAxis, float>> axes;
    float mouse_x;
    float mouse_y;
    int wheel;
    bool mouse_left;
    bool mouse_right;
    std::vector<SDL_Scancode> keys;
    bool reported;
};

// A plug or unplug line: the pad connects or disconnects at that VI.
struct PlugStep {
    uint64_t vi;
    bool plug;
    bool done;
};

struct VirtualPad {
    int number;  // 1 for the first script, ...
    std::vector<ScriptStep> script;
    std::vector<PlugStep> plugs;
    SDL_Joystick* joystick;
    Uint16 last_low, last_high, last_left, last_right;  // rumble, as last logged
};

std::vector<VirtualPad> g_virtual_pads;

// The virtual pad with that joystick, numbered from 1, or 0.
int virtual_pad_number(SDL_JoystickID id) {
    for (const VirtualPad& pad : g_virtual_pads) {
        if (pad.joystick != nullptr && SDL_JoystickInstanceID(pad.joystick) == id) {
            return pad.number;
        }
    }
    return 0;
}

bool load_script(const std::string& path, VirtualPad& pad) {
    std::ifstream file(path);
    if (!file) {
        std::fprintf(stderr, "[conker] CONKER_VIRTUAL_PAD: cannot open %s\n", path.c_str());
        return false;
    }
    std::string line;
    for (int number = 1; std::getline(file, line); number++) {
        line = line.substr(0, line.find('#'));
        std::istringstream in(line);
        std::string first;
        if (!(in >> first)) {
            continue;
        }
        if (first == "plug" || first == "unplug") {
            PlugStep plug{0, first == "plug", false};
            if (!(in >> plug.vi)) {
                std::fprintf(stderr, "[conker] CONKER_VIRTUAL_PAD line %d: %s needs a VI\n", number, first.c_str());
                return false;
            }
            pad.plugs.push_back(plug);
            continue;
        }
        ScriptStep step{};
        in.clear();
        in.seekg(0);
        if (!(in >> step.vi >> step.length)) {
            continue;
        }
        step.text = line;
        for (std::string control; in >> control;) {
            const size_t eq = control.find('=');
            const std::string name = control.substr(0, eq);
            if (control == "lmb" || control == "rmb") {
                (control == "lmb" ? step.mouse_left : step.mouse_right) = true;
            } else if (name == "key" && eq != std::string::npos) {
                const SDL_Scancode key = SDL_GetScancodeFromName(control.c_str() + eq + 1);
                if (key == SDL_SCANCODE_UNKNOWN) {
                    std::fprintf(stderr, "[conker] CONKER_VIRTUAL_PAD line %d: unknown key '%s'\n", number, control.c_str());
                    return false;
                }
                step.keys.push_back(key);
            } else if (name == "mousex" || name == "mousey" || name == "wheel") {
                const float value = std::strtof(control.c_str() + eq + 1, nullptr);
                if (name == "wheel") step.wheel = static_cast<int>(value);
                else (name == "mousex" ? step.mouse_x : step.mouse_y) = value;
            } else if (eq == std::string::npos) {
                const SDL_GameControllerButton button = SDL_GameControllerGetButtonFromString(control.c_str());
                if (button == SDL_CONTROLLER_BUTTON_INVALID) {
                    std::fprintf(stderr, "[conker] CONKER_VIRTUAL_PAD line %d: unknown button '%s'\n", number, control.c_str());
                    return false;
                }
                step.buttons.push_back(button);
            } else {
                const SDL_GameControllerAxis axis = SDL_GameControllerGetAxisFromString(control.substr(0, eq).c_str());
                if (axis == SDL_CONTROLLER_AXIS_INVALID) {
                    std::fprintf(stderr, "[conker] CONKER_VIRTUAL_PAD line %d: unknown axis '%s'\n", number, control.c_str());
                    return false;
                }
                step.axes.emplace_back(axis, std::strtof(control.c_str() + eq + 1, nullptr));
            }
        }
        pad.script.push_back(std::move(step));
    }
    return true;
}

// The rumble pak's on and off print as such; haptic effects on the motors print their strengths.
// "virtual pad" for the only one, "virtual pad 2" and so on when there are several, with the port
// it plays when that has another number (one unplugged before it frees a lower port).
std::string virtual_pad_name(int number) {
    std::string name = g_virtual_pads.size() == 1 ? "virtual pad" : "virtual pad " + std::to_string(number);
    for (const Port& port : g_ports) {
        if (port.pad != nullptr && port.virtual_number == number && port.number() != number) {
            name += " (controller " + std::to_string(port.number()) + ")";
        }
    }
    return name;
}
std::string virtual_pad_name(const VirtualPad& pad) { return virtual_pad_name(pad.number); }

int SDLCALL virtual_pad_rumble(void* userdata, Uint16 low, Uint16 high) {
    VirtualPad& pad = g_virtual_pads[reinterpret_cast<uintptr_t>(userdata)];
    if (low == pad.last_low && high == pad.last_high) {
        return 0;
    }
    pad.last_low = low;
    pad.last_high = high;
    const unsigned long long vi = conker::current_vi();
    const std::string name = virtual_pad_name(pad);
    if (low == high && (low == 0 || low == 0xFFFF)) {
        std::printf("[conker] %s: VI %llu: rumble %s\n", name.c_str(), vi, low != 0 ? "on" : "off");
    } else {
        std::printf("[conker] %s: VI %llu: rumble low %.2f high %.2f\n", name.c_str(), vi, low / 65535.0f, high / 65535.0f);
    }
    return 0;
}

int SDLCALL virtual_pad_rumble_triggers(void* userdata, Uint16 left, Uint16 right) {
    VirtualPad& pad = g_virtual_pads[reinterpret_cast<uintptr_t>(userdata)];
    if (left != pad.last_left || right != pad.last_right) {
        pad.last_left = left;
        pad.last_right = right;
        std::printf("[conker] %s: VI %llu: trigger rumble left %.2f right %.2f\n", virtual_pad_name(pad).c_str(),
                    static_cast<unsigned long long>(conker::current_vi()), left / 65535.0f, right / 65535.0f);
    }
    return 0;
}

// A strength or amplitude in [0, 1] as the DualSense's 1 to 8, or 0 for none.
int level(float value) { return static_cast<int>(std::lround(std::fmin(std::fmax(value, 0.0f), 1.0f) * 8.0f)); }

// A position in [0, 1] as one of the DualSense's ten zones along the pull.
int zone(float position) {
    return std::clamp(static_cast<int>(std::lround(position * kTriggerZones)), 0, kTriggerZones - 1);
}

// A DualSense trigger effect (11 bytes, as SDL's DS5EffectsState_t carries it).
void encode_trigger(const TriggerEffect& trigger, Uint8 effect[11]) {
    std::fill(effect, effect + 11, Uint8{0});
    uint16_t active = 0;
    uint32_t levels = 0;
    switch (trigger.mode) {
        case TriggerEffect::Mode::Feedback:
            for (int i = 0; i < kTriggerZones; i++) {
                if (const int strength = level(trigger.zones[i]); strength > 0) {
                    active |= static_cast<uint16_t>(1u << i);
                    levels |= static_cast<uint32_t>(strength - 1) << (3 * i);
                }
            }
            effect[0] = 0x21;
            break;
        case TriggerEffect::Mode::Weapon: {
            // The wall runs from zone 2 to 7 at the earliest and gives way by zone 8.
            const int first = std::clamp(zone(trigger.start), 2, 7);
            const int last = std::clamp(zone(trigger.end), first + 1, 8);
            const uint16_t zones = static_cast<uint16_t>((1u << first) | (1u << last));
            effect[0] = 0x25;
            effect[1] = static_cast<Uint8>(zones);
            effect[2] = static_cast<Uint8>(zones >> 8);
            effect[3] = static_cast<Uint8>(std::max(level(trigger.strength), 1) - 1);
            return;
        }
        case TriggerEffect::Mode::Vibration:
            if (const int amplitude = level(trigger.amplitude); amplitude > 0) {
                for (int i = zone(trigger.start); i < kTriggerZones; i++) {
                    active |= static_cast<uint16_t>(1u << i);
                    levels |= static_cast<uint32_t>(amplitude - 1) << (3 * i);
                }
            }
            effect[0] = 0x26;
            effect[9] = static_cast<Uint8>(std::lround(1.0f + std::fmin(std::fmax(trigger.frequency, 0.0f), 1.0f) * 254.0f));  // Hz
            break;
        case TriggerEffect::Mode::Off:
            break;
    }
    if (active == 0) {
        std::fill(effect, effect + 11, Uint8{0});
        effect[0] = 0x05;  // off
        return;
    }
    effect[1] = static_cast<Uint8>(active);
    effect[2] = static_cast<Uint8>(active >> 8);
    for (int i = 0; i < 4; i++) {
        effect[3 + i] = static_cast<Uint8>(levels >> (8 * i));
    }
}

// The fields of SDL's DS5EffectsState_t up to the trigger effects.
struct DualSenseEffects {
    Uint8 enable_bits1;  // 0x04 right trigger, 0x08 left trigger
    Uint8 enable_bits2;
    Uint8 unused[8];
    Uint8 right_trigger[11];
    Uint8 left_trigger[11];
};
static_assert(sizeof(DualSenseEffects) == 32);

int SDLCALL virtual_pad_effect(void* userdata, const void* data, int size) {
    if (size < static_cast<int>(sizeof(DualSenseEffects))) {
        return -1;
    }
    const VirtualPad& pad = g_virtual_pads[reinterpret_cast<uintptr_t>(userdata)];
    const auto* effects = static_cast<const DualSenseEffects*>(data);
    std::printf("[conker] %s: VI %llu: triggers", virtual_pad_name(pad).c_str(),
                static_cast<unsigned long long>(conker::current_vi()));
    for (Uint8 byte : effects->right_trigger) std::printf(" %02x", byte);
    std::printf("\n");
    return 0;
}

// Sends the effect to the port's pad: SDL's own DualSense driver takes the effect report, and on
// macOS a pad read through Apple's framework is set there.
bool send_triggers(Port& port, const TriggerEffect& trigger) {
    DualSenseEffects effects{};
    effects.enable_bits1 = 0x04 | 0x08;
    encode_trigger(trigger, effects.right_trigger);
    encode_trigger(trigger, effects.left_trigger);
    // sdl2-compat reports a virtual pad's effect as failed even when the pad takes it.
    if (SDL_GameControllerSendEffect(port.pad, &effects, sizeof(effects)) == 0 || port.pad_virtual) {
        return true;
    }
#ifdef __APPLE__
    return apple::set_dualsense_triggers(port.number() - 1, trigger);
#else
    return false;
#endif
}

// Leaves the triggers free when the game quits: a DualSense keeps an effect until told otherwise.
void release_triggers() {
    for (Port& port : g_ports) {
        if (port.pad != nullptr && port.adaptive_triggers.load() && !(port.trigger_applied == TriggerEffect{})) {
            send_triggers(port, TriggerEffect{});
        }
    }
}

bool plug_virtual_pad(VirtualPad& pad) {
    SDL_VirtualJoystickDesc desc{};
    desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    desc.naxes = SDL_CONTROLLER_AXIS_MAX;
    desc.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
    desc.button_mask = (1u << SDL_CONTROLLER_BUTTON_MAX) - 1;
    desc.axis_mask = (1u << SDL_CONTROLLER_AXIS_MAX) - 1;
    desc.name = "Conker virtual pad";
    // CONKER_VIRTUAL_PAD_ID=<vendor>:<product> (hex) and CONKER_VIRTUAL_PAD_NAME=<text>: the pad poses as
    // another make with a name of its own, to check that only its make and model leave this machine.
    const char* pose_id = std::getenv("CONKER_VIRTUAL_PAD_ID");
    const char* pose_name = std::getenv("CONKER_VIRTUAL_PAD_NAME");
    unsigned vendor = 0;
    unsigned product = 0;
    if (pose_id != nullptr && std::sscanf(pose_id, "%x:%x", &vendor, &product) == 2) {
        desc.vendor_id = static_cast<uint16_t>(vendor);
        desc.product_id = static_cast<uint16_t>(product);
    }
    if (pose_name != nullptr && pose_name[0] != '\0') {
        desc.name = pose_name;
    }
    desc.userdata = reinterpret_cast<void*>(static_cast<uintptr_t>(pad.number - 1));
    desc.Rumble = virtual_pad_rumble;
    desc.RumbleTriggers = virtual_pad_rumble_triggers;
    desc.SendEffect = virtual_pad_effect;
    const int index = SDL_JoystickAttachVirtualEx(&desc);
    pad.joystick = index < 0 ? nullptr : SDL_JoystickOpen(index);
    if (pad.joystick == nullptr) {
        std::fprintf(stderr, "[conker] CONKER_VIRTUAL_PAD: %s\n", SDL_GetError());
        return false;
    }
    return true;
}

void unplug_virtual_pad(VirtualPad& pad) {
    const SDL_JoystickID id = SDL_JoystickInstanceID(pad.joystick);
    SDL_JoystickClose(pad.joystick);
    pad.joystick = nullptr;
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (SDL_JoystickGetDeviceInstanceID(i) == id) {
            SDL_JoystickDetachVirtual(i);
            return;
        }
    }
}

// CONKER_VIRTUAL_PAD=<script>[,<script>...]: one virtual pad per script, connected in that order
// (so the first takes controller 1), each from the start unless its first plug line plugs it in.
void attach_virtual_pad() {
    const char* paths = std::getenv("CONKER_VIRTUAL_PAD");
    if (paths == nullptr || *paths == '\0') {
        return;
    }
    std::istringstream list(paths);
    for (std::string path; std::getline(list, path, ',');) {
        VirtualPad pad{};
        pad.number = static_cast<int>(g_virtual_pads.size()) + 1;
        if (!load_script(path, pad)) {
            return;
        }
        std::printf("[conker] virtual pad %d: %zu scripted steps from %s\n", pad.number, pad.script.size(), path.c_str());
        g_virtual_pads.push_back(std::move(pad));
    }
    for (VirtualPad& pad : g_virtual_pads) {
        if (pad.plugs.empty() || !pad.plugs.front().plug) {
            plug_virtual_pad(pad);
        }
    }
    g_scripted = !g_virtual_pads.empty();
}

// Takes effect when SDL next updates joysticks, i.e. on the next event pump.
void drive_virtual_pad() {
    const uint64_t now = conker::current_vi();
    for (VirtualPad& pad : g_virtual_pads) {
        for (PlugStep& plug : pad.plugs) {
            if (plug.done || now < plug.vi) {
                continue;
            }
            plug.done = true;
            std::printf("[conker] %s: VI %llu: %s\n", virtual_pad_name(pad).c_str(), static_cast<unsigned long long>(now),
                        plug.plug ? "plugged in" : "unplugged");
            if (plug.plug && pad.joystick == nullptr) {
                plug_virtual_pad(pad);
            } else if (!plug.plug && pad.joystick != nullptr) {
                unplug_virtual_pad(pad);
            }
        }
        if (pad.joystick == nullptr) {
            continue;
        }
        bool buttons[SDL_CONTROLLER_BUTTON_MAX] = {};
        float axes[SDL_CONTROLLER_AXIS_MAX] = {};
        const bool first = &pad == &g_virtual_pads.front();
        static uint64_t mouse_vi = 0;  // the scripted mouse moves by VI, however often this runs
        const float mouse_vis = first && now > mouse_vi ? static_cast<float>(now - mouse_vi) : 0.0f;
        if (first) {
            mouse_vi = std::max(mouse_vi, now);
            g_script_mouse_left = false;
            g_script_mouse_right = false;
            g_script_keys.clear();
        }
        for (ScriptStep& step : pad.script) {
            if (now < step.vi || now >= step.vi + step.length) {
                continue;
            }
            if (!step.reported) {
                step.reported = true;
                std::printf("[conker] %s: VI %llu:%s\n", virtual_pad_name(pad).c_str(), static_cast<unsigned long long>(now),
                            step.text.c_str());
                if (first && step.wheel != 0) add_zoom(step.wheel > 0 ? 1 : -1);
            }
            for (SDL_GameControllerButton b : step.buttons) buttons[b] = true;
            for (const auto& [a, v] : step.axes) axes[a] = v;
            if (first) {
                g_script_mouse_left = g_script_mouse_left || step.mouse_left;
                g_script_mouse_right = g_script_mouse_right || step.mouse_right;
                g_script_keys.insert(g_script_keys.end(), step.keys.begin(), step.keys.end());
                if (g_mouse_captured && (step.mouse_x != 0.0f || step.mouse_y != 0.0f)) {
                    std::lock_guard lock(g_mouse_mutex);
                    g_mouse_dx += step.mouse_x * mouse_vis;
                    g_mouse_dy += step.mouse_y * mouse_vis;
                }
            }
        }
        for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; b++) {
            SDL_JoystickSetVirtualButton(pad.joystick, b, buttons[b] ? SDL_PRESSED : SDL_RELEASED);
        }
        for (int a = 0; a < SDL_CONTROLLER_AXIS_MAX; a++) {
            // A trigger's raw axis runs from -32768 (released) to 32767, which SDL maps to 0..32767; a
            // raw 0 would be half pressed, which is Z.
            const bool trigger = a == SDL_CONTROLLER_AXIS_TRIGGERLEFT || a == SDL_CONTROLLER_AXIS_TRIGGERRIGHT;
            const float v = std::fmax(trigger ? 0.0f : -1.0f, std::fmin(axes[a], 1.0f));
            const float raw = trigger ? v * 65535.0f - 32768.0f : v * 32767.0f;
            SDL_JoystickSetVirtualAxis(pad.joystick, a, static_cast<Sint16>(raw));
        }
    }
}

float axis(SDL_GameController* pad, SDL_GameControllerAxis which) {
    return static_cast<float>(SDL_GameControllerGetAxis(pad, which)) / 32767.0f;
}

// A stick's position past the radial deadzone, rescaled so full deflection still reaches the
// edge; inside it, x and y are left as they are. SDL's +y is down, so y is negated.
void deadzone(SDL_GameController* pad, SDL_GameControllerAxis axis_x, SDL_GameControllerAxis axis_y, float& x, float& y) {
    const float px = axis(pad, axis_x);
    const float py = -axis(pad, axis_y);
    const float magnitude = std::sqrt(px * px + py * py);
    if (magnitude > kStickDeadzone) {
        const float scale = std::fmin((magnitude - kStickDeadzone) / (1.0f - kStickDeadzone), 1.0f) / magnitude;
        x = px * scale;
        y = py * scale;
    }
}

// The controls a pad holds. A trigger counts once pulled past z_at; left_trigger and right_trigger
// say which are. The right stick counts pushed half way.
Controls held_controls(SDL_GameController* pad, float z_at, bool& left_trigger, bool& right_trigger) {
    Controls held = 0;
    for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; b++) {
        if (SDL_GameControllerGetButton(pad, static_cast<SDL_GameControllerButton>(b))) {
            held |= bit(b);
        }
    }
    const auto threshold = static_cast<Sint16>(z_at * 32767.0f);
    left_trigger = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > threshold;
    right_trigger = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > threshold;
    if (left_trigger) held |= bit(kLeftTrigger);
    if (right_trigger) held |= bit(kRightTrigger);
    const Sint16 cx = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTX);
    const Sint16 cy = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTY);
    if (cx < -kCButtonThreshold) held |= bit(kRightStickLeft);
    if (cx > kCButtonThreshold) held |= bit(kRightStickRight);
    if (cy < -kCButtonThreshold) held |= bit(kRightStickUp);  // SDL's +y is down
    if (cy > kCButtonThreshold) held |= bit(kRightStickDown);
    return held;
}

// The game's buttons the held controls press, as a mapping has them.
uint16_t mapped_buttons(Controls held, const Controls* mapping) {
    uint16_t buttons = 0;
    for (int i = 0; i < kMappedButtonCount; i++) {
        if ((held & mapping[i]) != 0) {
            buttons |= kMappedButtons[i].n64;
        }
    }
    return buttons;
}

// Adds a pad's controls to `held`, and its left stick to x and y and its right stick to right_x
// and right_y when outside the deadzone.
void read_pad(SDL_GameController* pad, float z_at, Controls& held, float& x, float& y, float& right_x,
              float& right_y, bool& left_trigger, bool& right_trigger) {
    held |= held_controls(pad, z_at, left_trigger, right_trigger);
    deadzone(pad, SDL_CONTROLLER_AXIS_LEFTX, SDL_CONTROLLER_AXIS_LEFTY, x, y);
    deadzone(pad, SDL_CONTROLLER_AXIS_RIGHTX, SDL_CONTROLLER_AXIS_RIGHTY, right_x, right_y);
}

// C-left and C-right back the N64's way round when camera_swapped is off.
uint16_t unswap_camera(uint16_t buttons) {
    if (g_camera_swapped.load(std::memory_order_relaxed)) {
        return buttons;
    }
    const uint16_t c = buttons & (kCLeft | kCRight);
    return static_cast<uint16_t>((buttons & ~(kCLeft | kCRight)) | ((c & kCLeft) ? kCRight : 0) | ((c & kCRight) ? kCLeft : 0));
}

void store(Port& port, uint16_t buttons, float x, float y, float right_x, float right_y) {
    port.buttons.store(unswap_camera(buttons), std::memory_order_relaxed);
    port.stick_x.store(x, std::memory_order_relaxed);
    port.stick_y.store(y, std::memory_order_relaxed);
    port.right_x.store(right_x, std::memory_order_relaxed);
    port.right_y.store(right_y, std::memory_order_relaxed);
    port.has_pad.store(port.pad != nullptr, std::memory_order_relaxed);
}

// --- haptic effects on the rumble motors ---
// A tap lasts long enough for the motor to spin up, longer the softer it is. A Grip::Both effect
// leans on the light motor the sharper it is and on the heavy one the softer: both run at full
// strength at sharpness 0.5. `on_trigger` is a Grip::Trigger effect, resolved to haptic.grip's side.
MotorEffect motor_effect(const Port& port, const Haptic& haptic, bool on_trigger, Uint64 now) {
    const float sharpness = std::clamp(haptic.sharpness, 0.0f, 1.0f);
    const float intensity = std::clamp(haptic.intensity, 0.0f, 1.0f);
    const float seconds = haptic.duration > 0.0f ? haptic.duration : kSoftTap + (kSharpTap - kSoftTap) * sharpness;
    MotorEffect effect{now + static_cast<Uint64>(seconds * 1000.0f + 0.5f), 0.0f, 0.0f, 0.0f, 0.0f};
    if (on_trigger && port.has_trigger_rumble) {
        (haptic.grip == Haptic::Grip::Left ? effect.left_trigger : effect.right_trigger) = intensity;
        return effect;
    }
    switch (haptic.grip) {
        case Haptic::Grip::Left:
            effect.low = intensity;
            break;
        case Haptic::Grip::Right:
            effect.high = intensity;
            break;
        default:
            effect.low = intensity * std::fmin(1.0f, 2.0f * (1.0f - sharpness));
            effect.high = intensity * std::fmin(1.0f, 2.0f * sharpness);
            break;
    }
    return effect;
}

Uint16 motor_strength(float level) { return static_cast<Uint16>(std::clamp(level, 0.0f, 1.0f) * 65535.0f + 0.5f); }

// Sets a port's motors from the rumble pak and the haptic effects still playing, the strongest of
// them for each motor. The pak's motor is on or off and drives both grip motors at full strength (on
// a DualSense through Core Haptics, its own rumble in both grips instead). A
// motor set for an effect stops by itself when the next effect ends, so a late update cuts an
// effect short rather than holding it; the pak's is held kRumbleHoldMs, so a stalled game stops it.
// SDL only renews the expiry when the strength is unchanged, so this costs nothing per frame.
void apply_rumble(Port& port, Uint64 now) {
    std::erase_if(port.motor_effects, [now](const MotorEffect& effect) { return effect.end_ms <= now; });
    if (port.pad == nullptr || !port.has_rumble.load(std::memory_order_relaxed)) {
        port.motor_effects.clear();
        return;
    }
    float low = 0.0f;
    float high = 0.0f;
    float left_trigger = 0.0f;
    float right_trigger = 0.0f;
    Uint64 next_end = now + kRumbleHoldMs;
    Uint64 next_trigger_end = now + kRumbleHoldMs;
    for (const MotorEffect& effect : port.motor_effects) {
        low = std::fmax(low, effect.low);
        high = std::fmax(high, effect.high);
        left_trigger = std::fmax(left_trigger, effect.left_trigger);
        right_trigger = std::fmax(right_trigger, effect.right_trigger);
        if (effect.low > 0.0f || effect.high > 0.0f) next_end = std::min(next_end, effect.end_ms);
        if (effect.left_trigger > 0.0f || effect.right_trigger > 0.0f) next_trigger_end = std::min(next_trigger_end, effect.end_ms);
    }
    bool pak = port.rumble_on.load(std::memory_order_relaxed);
#ifdef __APPLE__
    // A DualSense played through Core Haptics takes the pak as a deep rumble in both grips: SDL's
    // own rumble on it is a thin buzz.
    if (port.haptics_via == HapticsVia::CoreHaptics) {
        if (pak != port.pak_rumble_applied && apple::set_dualsense_rumble(port.number() - 1, pak)) {
            port.pak_rumble_applied = pak;
        }
        if (port.pak_rumble_applied == pak) {
            pak = false;
        }
    }
#endif
    const Uint16 low_strength = pak ? kRumbleStrength : motor_strength(low);
    const Uint16 high_strength = pak ? kRumbleStrength : motor_strength(high);
    const Uint32 hold = pak ? kRumbleHoldMs : static_cast<Uint32>(next_end - now);
    const bool on = low_strength != 0 || high_strength != 0;
    int result = 0;
    if (on) {
        result = SDL_GameControllerRumble(port.pad, low_strength, high_strength, hold);
    } else if (port.rumble_applied) {
        result = SDL_GameControllerRumble(port.pad, 0, 0, 0);
    }
    port.rumble_applied = on;

    if (port.has_trigger_rumble) {
        const Uint16 left_strength = motor_strength(left_trigger);
        const Uint16 right_strength = motor_strength(right_trigger);
        const bool triggers_on = left_strength != 0 || right_strength != 0;
        int trigger_result = 0;
        if (triggers_on) {
            trigger_result = SDL_GameControllerRumbleTriggers(port.pad, left_strength, right_strength,
                                                              static_cast<Uint32>(next_trigger_end - now));
        } else if (port.trigger_rumble_applied) {
            trigger_result = SDL_GameControllerRumbleTriggers(port.pad, 0, 0, 0);
        }
        port.trigger_rumble_applied = triggers_on;
        result = result != 0 ? result : trigger_result;
    }
    static bool reported[kPorts] = {};
    // sdl2-compat reports a virtual pad's rumble as failed (with no error) even when it takes it.
    if (result != 0 && !port.pad_virtual && !reported[port.number() - 1]) {
        reported[port.number() - 1] = true;
        std::fprintf(stderr, "[conker] controller %d rumble failed: %s\n", port.number(), SDL_GetError());
    }
}

// Plays the haptic effects waiting for a port's pad, as its pad plays them.
void play_haptics(Port& port, const std::vector<Haptic>& haptics, Uint64 now) {
    if (port.pad == nullptr || !port.has_haptics.load(std::memory_order_relaxed)) {
        return;
    }
    for (Haptic haptic : haptics) {
        const bool on_trigger = haptic.grip == Haptic::Grip::Trigger;
        if (on_trigger) {
            haptic.grip = port.trigger_grip;
        }
        if (port.haptics_via == HapticsVia::Log) {
            static const char* const kGrips[] = {"left", "right", "both"};
            std::printf("[conker] %s: VI %llu: haptic %.2f sharpness %.2f for %.2f s, %s\n",
                        virtual_pad_name(port.virtual_number).c_str(), static_cast<unsigned long long>(conker::current_vi()),
                        haptic.intensity, haptic.sharpness, haptic.duration, kGrips[static_cast<int>(haptic.grip)]);
            continue;
        }
#ifdef __APPLE__
        // No DualSense among the GameController framework's pads (SDL_JOYSTICK_HIDAPI_PS5=1 reads
        // it through SDL's own driver): the motors, which SDL rumbles.
        if (port.haptics_via == HapticsVia::CoreHaptics && apple::play_dualsense_haptic(port.number() - 1, haptic)) {
            continue;
        }
#endif
        port.motor_effects.push_back(motor_effect(port, haptic, on_trigger, now));
    }
}

// Sends the port's wanted trigger effect when it has changed.
void apply_triggers(Port& port) {
    if (port.pad == nullptr || !port.adaptive_triggers.load(std::memory_order_relaxed)) {
        return;
    }
    TriggerEffect wanted;
    {
        std::lock_guard lock(g_trigger_mutex);
        wanted = port.trigger_wanted;
    }
    if (port.trigger_stale || !(wanted == port.trigger_applied)) {
        static bool reported[kPorts] = {};
        if (!send_triggers(port, wanted) && !reported[port.number() - 1]) {
            reported[port.number() - 1] = true;
            std::fprintf(stderr, "[conker] controller %d adaptive triggers failed: %s\n", port.number(), SDL_GetError());
        }
        port.trigger_applied = wanted;
        port.trigger_stale = false;
    }
}

// Reads a port's pad into buttons and the sticks, with Z where its trigger effect puts it, and
// notes the trigger last pulled for Grip::Trigger.
// --- the button mapping ---------------------------------------------------------------------------

// A control's name in input.cfg: SDL's for a button ("a", "leftshoulder"), else these.
std::string control_key(int control) {
    switch (control) {
        case kLeftTrigger: return "lefttrigger";
        case kRightTrigger: return "righttrigger";
        case kRightStickUp: return "rightstickup";
        case kRightStickDown: return "rightstickdown";
        case kRightStickLeft: return "rightstickleft";
        case kRightStickRight: return "rightstickright";
        default: {
            const char* name = SDL_GameControllerGetStringForButton(static_cast<SDL_GameControllerButton>(control));
            return name != nullptr ? name : "";
        }
    }
}

int control_from_key(const std::string& key) {
    for (int control = 0; control < kControlCount; control++) {
        if (control_key(control) == key) {
            return control;
        }
    }
    return -1;
}

std::string mapping_key(int port) { return "map" + std::to_string(port + 1); }

// "a:a;b:x,b;...": each of kMappedButtons and its controls.
std::string mapping_text(int port) {
    std::string text;
    for (int i = 0; i < kMappedButtonCount; i++) {
        text += std::string(i == 0 ? "" : ";") + kMappedButtons[i].key + ":";
        const Controls controls = g_mapping[port][i].load();
        bool first = true;
        for (int control = 0; control < kControlCount; control++) {
            if ((controls & bit(control)) != 0) {
                text += (first ? "" : ",") + control_key(control);
                first = false;
            }
        }
    }
    return text;
}

// The ports' mappings from input.cfg; a button it does not name keeps its default, and a control it
// does not know (another SDL's) is left out.
// "a:44;b:8,...": each of kMappedButtons and its keys' scancodes.
std::string key_mapping_text() {
    std::string text;
    std::lock_guard lock(g_key_mutex);
    for (int i = 0; i < kKeyRows; i++) {
        text += std::string(i == 0 ? "" : ";") + row_key(i) + ":";
        for (size_t k = 0; k < g_key_mapping[i].size(); k++) {
            text += (k == 0 ? "" : ",") + std::to_string(g_key_mapping[i][k]);
        }
    }
    return text;
}

void load_key_mapping() {
    {
        std::lock_guard lock(g_key_mutex);
        for (int i = 0; i < kKeyRows; i++) {
            g_key_mapping[i] = default_key_mapping(i);
        }
        std::stringstream text(input_config::get("keys"));
        for (std::string entry; std::getline(text, entry, ';');) {
            const size_t colon = entry.find(':');
            for (int i = 0; colon != std::string::npos && i < kKeyRows; i++) {
                if (entry.substr(0, colon) != row_key(i)) {
                    continue;
                }
                g_key_mapping[i].clear();
                std::stringstream keys(entry.substr(colon + 1));
                for (std::string key; std::getline(keys, key, ',');) {
                    const int scancode = std::atoi(key.c_str());
                    if (key_mappable(scancode)) g_key_mapping[i].push_back(scancode);
                }
            }
        }
    }
    if (!key_mapping_is_default()) {
        std::printf("[conker] keyboard: keys mapped (%s)\n", key_mapping_text().c_str());
    }
}

void load_mapping() {
    load_key_mapping();
    for (int port = 0; port < kPorts; port++) {
        for (int i = 0; i < kMappedButtonCount; i++) {
            g_mapping[port][i].store(kDefaultMapping[i]);
        }
        std::stringstream text(input_config::get(mapping_key(port)));
        for (std::string entry; std::getline(text, entry, ';');) {
            const size_t colon = entry.find(':');
            if (colon == std::string::npos) {
                continue;
            }
            const std::string key = entry.substr(0, colon);
            for (int i = 0; i < kMappedButtonCount; i++) {
                if (key != kMappedButtons[i].key) {
                    continue;
                }
                Controls controls = 0;
                std::stringstream names(entry.substr(colon + 1));
                for (std::string name; std::getline(names, name, ',');) {
                    const int control = control_from_key(name);
                    if (control >= 0) {
                        controls |= bit(control);
                    }
                }
                g_mapping[port][i].store(controls & ~kUnmappable);
            }
        }
        if (!mapping_is_default(port)) {
            std::printf("[conker] controller %d: buttons mapped (%s)\n", port + 1, mapping_text(port).c_str());
        }
    }
}

// A control's name on a kind of pad, for the game's font.
std::string control_name(int control, PadKind kind) {
    const bool playstation = kind == PadKind::PlayStation;
    switch (control) {
        case SDL_CONTROLLER_BUTTON_A: return playstation ? "CROSS" : "A";
        case SDL_CONTROLLER_BUTTON_B: return playstation ? "CIRCLE" : "B";
        case SDL_CONTROLLER_BUTTON_X: return playstation ? "SQUARE" : "X";
        case SDL_CONTROLLER_BUTTON_Y: return playstation ? "TRIANGLE" : "Y";
        case SDL_CONTROLLER_BUTTON_BACK: return playstation ? "CREATE" : "VIEW";
        case SDL_CONTROLLER_BUTTON_GUIDE: return playstation ? "PS" : "GUIDE";
        case SDL_CONTROLLER_BUTTON_START: return playstation ? "OPTIONS" : "START";
        case SDL_CONTROLLER_BUTTON_LEFTSTICK: return playstation ? "L3" : "LEFT STICK";
        case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return playstation ? "R3" : "RIGHT STICK";
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return playstation ? "L1" : "LB";
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return playstation ? "R1" : "RB";
        case SDL_CONTROLLER_BUTTON_DPAD_UP: return "D-PAD UP";
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return "D-PAD DOWN";
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return "D-PAD LEFT";
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return "D-PAD RIGHT";
        case SDL_CONTROLLER_BUTTON_MISC1: return playstation ? "MIC" : "SHARE";
        case SDL_CONTROLLER_BUTTON_PADDLE1: return "PADDLE 1";
        case SDL_CONTROLLER_BUTTON_PADDLE2: return "PADDLE 2";
        case SDL_CONTROLLER_BUTTON_PADDLE3: return "PADDLE 3";
        case SDL_CONTROLLER_BUTTON_PADDLE4: return "PADDLE 4";
        case SDL_CONTROLLER_BUTTON_TOUCHPAD: return "TOUCHPAD";
        case kLeftTrigger: return playstation ? "L2" : "LT";
        case kRightTrigger: return playstation ? "R2" : "RT";
        case kRightStickUp: return "RIGHT STICK UP";
        case kRightStickDown: return "RIGHT STICK DOWN";
        case kRightStickLeft: return "RIGHT STICK LEFT";
        case kRightStickRight: return "RIGHT STICK RIGHT";
        default: return "BUTTON " + std::to_string(control);
    }
}

// Listening for a control (start_listening): the port, or -1, and what was heard. The controls held
// as it started (and still held) are main thread only.
std::atomic<int> g_listen_port{-1};
std::atomic<bool> g_listen_fresh{false};
std::atomic<Controls> g_heard{0};
Controls g_listen_held = 0;

// Once an update for each port with a pad: what it holds, for a control pressed while listening.
void listen(int port, Controls held) {
    if (g_listen_port.load(std::memory_order_relaxed) != port) {
        return;
    }
    held &= ~kUnmappable;
    if (g_listen_fresh.exchange(false)) {
        g_listen_held = held;
        return;
    }
    const Controls pressed = held & ~g_listen_held;
    g_listen_held &= held;
    if (pressed != 0) {
        g_heard.store(pressed & (~pressed + 1));  // the lowest, if several came in one update
        g_listen_port.store(-1);
    }
}

// Listening for a key (start_listening_keys); the keys held as it started are main thread only.
std::atomic<bool> g_listen_keys{false};
std::atomic<bool> g_listen_keys_fresh{false};
std::atomic<int> g_heard_key{0};
std::vector<int> g_listen_keys_held;

// Once an update, with the keyboard's state: a key newly pressed while listening.
void listen_keys(const Uint8* keys, int key_count) {
    if (!g_listen_keys.load(std::memory_order_relaxed)) {
        return;
    }
    std::vector<int> held;
    for (int key = 0; key < key_count; key++) {
        if (keys[key] && key_mappable(key)) held.push_back(key);
    }
    for (SDL_Scancode key : g_script_keys) {
        if (key_mappable(key)) held.push_back(key);
    }
    if (g_listen_keys_fresh.exchange(false)) {
        g_listen_keys_held = held;
        return;
    }
    std::erase_if(g_listen_keys_held, [&](int key) { return std::find(held.begin(), held.end(), key) == held.end(); });
    for (int key : held) {
        if (std::find(g_listen_keys_held.begin(), g_listen_keys_held.end(), key) == g_listen_keys_held.end()) {
            g_heard_key.store(key);
            g_listen_keys.store(false);
            return;
        }
    }
}

// Adds the port's pad's buttons, as the port's mapping has them, to `buttons` (and as the default
// mapping has them to `menu_buttons`, when given), and its sticks as read_pad does.
void read_port_pad(Port& port, uint16_t& buttons, float& x, float& y, float& right_x, float& right_y,
                   uint16_t* menu_buttons = nullptr) {
    // A trigger effect on adaptive triggers can move how far they are pulled for Z.
    float z_at = kTriggerZ;
    if (port.adaptive_triggers.load(std::memory_order_relaxed)) {
        std::lock_guard lock(g_trigger_mutex);
        z_at = port.trigger_wanted.z_at;
    }
    bool left_trigger = false;
    bool right_trigger = false;
    Controls held = 0;
    read_pad(port.pad, z_at, held, x, y, right_x, right_y, left_trigger, right_trigger);
    Controls mapping[kMappedButtonCount];
    const int index = port.number() - 1;
    for (int i = 0; i < kMappedButtonCount; i++) {
        mapping[i] = g_mapping[index][i].load(std::memory_order_relaxed);
    }
    buttons |= mapped_buttons(held, mapping);
    if (menu_buttons != nullptr) {
        *menu_buttons |= mapped_buttons(held, kDefaultMapping);
    }
    listen(index, held);
    if (left_trigger && !port.left_trigger_held) port.trigger_grip = Haptic::Grip::Left;
    if (right_trigger && !port.right_trigger_held) port.trigger_grip = Haptic::Grip::Right;
    port.left_trigger_held = left_trigger;
    port.right_trigger_held = right_trigger;
}

} // namespace

void init() {
    load_mapping();
    // Keep reading the pad while another window (a terminal, a debugger) has focus.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
#ifdef __APPLE__
    // PlayStation pads through Apple's GameController framework rather than SDL's HIDAPI driver.
    // HIDAPI's rumble switches a Bluetooth pad to extended reports, which it keeps until it is
    // power-cycled, so other apps (OpenEmu) can't read it after we quit. GameController rumbles
    // through Core Haptics without that. An environment variable of the same name overrides this.
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5, "0");
#endif
    // Extended reports: rumble on PlayStation pads over Bluetooth.
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4_RUMBLE, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, "1");
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
        std::fprintf(stderr, "[conker] no controller support (%s); keyboard only\n", SDL_GetError());
        return;
    }
    attach_virtual_pad();
    open_all_pads();
    std::atexit(release_triggers);
}

void handle_event(const SDL_Event& event) {
    mouse_event(event);
    switch (event.type) {
        case SDL_CONTROLLERDEVICEADDED:
            open_pad(event.cdevice.which);
            break;
        case SDL_CONTROLLERDEVICEREMOVED:
            for (Port& port : g_ports) {
                if (port.pad != nullptr && event.cdevice.which == port.id) {
                    close_pad(port);
                    open_all_pads();  // a fifth pad, waiting for a port
                    break;
                }
            }
            break;
        default:
            break;
    }
}

void update() {
    drive_virtual_pad();

    uint16_t buttons = 0;
    float x = 0.0f;
    float y = 0.0f;
    float right_x = 0.0f;
    float right_y = 0.0f;
    bool debug_menu = false;

    // Keys held with Cmd or Alt are shortcuts (Cmd+Enter toggles full screen), not game input.
    uint16_t menu_keys = 0;  // as the default keys have them, for read_menu
    bool zoom_in = false;
    bool zoom_out = false;
    const Uint8* game_keys = nullptr;
    int key_count = 0;
    if ((SDL_GetModState() & (KMOD_GUI | KMOD_ALT)) == 0) {
        const Uint8* keys = SDL_GetKeyboardState(&key_count);
        game_keys = keys;
        auto held = [&](int key) {
            return (key < key_count && keys[key]) ||
                   std::find(g_script_keys.begin(), g_script_keys.end(), key) != g_script_keys.end();
        };
        for (const Key& k : kKeys) {
            if (held(k.key)) menu_keys |= k.n64;
        }
        bool row_held[kKeyRows] = {};
        {
            std::lock_guard lock(g_key_mutex);
            for (int i = 0; i < kKeyRows; i++) {
                for (int key : g_key_mapping[i]) {
                    row_held[i] = row_held[i] || held(key);
                }
                if (i < kMappedButtonCount && row_held[i]) buttons |= kMappedButtons[i].n64;
            }
        }
        listen_keys(keys, key_count);
        x = static_cast<float>(int(row_held[kStickRight]) - int(row_held[kStickLeft]));
        y = static_cast<float>(int(row_held[kStickUp]) - int(row_held[kStickDown]));
        if (row_held[kWalk]) {
            x *= kWalkTilt;
            y *= kWalkTilt;
        }
        zoom_in = row_held[kZoomIn];
        zoom_out = row_held[kZoomOut];
        debug_menu = keys[SDL_SCANCODE_GRAVE] != 0;
    }
    uint16_t menu_buttons = menu_keys;
    update_mouse(buttons, zoom_in, zoom_out);

    if (g_port1.pad != nullptr) {
        debug_menu = debug_menu || SDL_GameControllerGetButton(g_port1.pad, SDL_CONTROLLER_BUTTON_BACK);
        const float key_x = x;
        const float key_y = y;
        uint16_t pad_menu = 0;
        read_port_pad(g_port1, buttons, x, y, right_x, right_y, &pad_menu);
        menu_buttons |= pad_menu;
        g_menu_pad_buttons.store(pad_menu, std::memory_order_relaxed);
        // The stick unless the keys that move it are held (typing a name with W A S D in the lobby).
        const bool keys_move = key_x != 0.0f || key_y != 0.0f;
        g_menu_pad_x.store(keys_move ? 0.0f : x, std::memory_order_relaxed);
        g_menu_pad_y.store(keys_move ? 0.0f : y, std::memory_order_relaxed);
        // A pad's stick moved: the player is on the pad, so the mouse is let go (a click takes it
        // again). While it is held, a match reads the stick's left and right as A and D, stepping
        // sideways as in a shooter, which turned the pad's stick into a strafe after a click on the
        // window.
        constexpr float kPadInUse = 0.3f;
        if (g_mouse_captured && !g_scripted &&
            (std::fabs(x - key_x) > kPadInUse || std::fabs(y - key_y) > kPadInUse ||
             std::fabs(right_x) > kPadInUse || std::fabs(right_y) > kPadInUse)) {
            capture_mouse(false);
            g_mouse_recapture = false;
        }
    }
    if (g_port1.pad == nullptr) {
        g_menu_pad_buttons.store(0, std::memory_order_relaxed);
        g_menu_pad_x.store(0.0f, std::memory_order_relaxed);
        g_menu_pad_y.store(0.0f, std::memory_order_relaxed);
    }
    store(g_port1, buttons, x, y, right_x, right_y);
    g_menu_buttons.store(menu_buttons, std::memory_order_relaxed);
    if (debug_menu && !g_debug_menu_held) {
        g_debug_menu_presses.fetch_add(1, std::memory_order_relaxed);
    }
    g_debug_menu_held = debug_menu;

    for (int i = 1; i < kPorts; i++) {
        Port& port = g_ports[i];
        if (port.pad != nullptr) {
            uint16_t port_buttons = 0;
            float port_x = 0.0f;
            float port_y = 0.0f;
            float port_right_x = 0.0f;
            float port_right_y = 0.0f;
            read_port_pad(port, port_buttons, port_x, port_y, port_right_x, port_right_y);
            store(port, port_buttons, port_x, port_y, port_right_x, port_right_y);
        }
    }

    std::vector<Haptic> haptics[kPorts];
    {
        std::lock_guard lock(g_haptics_mutex);
        for (int i = 0; i < kPorts; i++) haptics[i].swap(g_ports[i].haptics);
    }
    const Uint64 now = SDL_GetTicks64();
    for (int i = 0; i < kPorts; i++) {
        play_haptics(g_ports[i], haptics[i], now);
        apply_rumble(g_ports[i], now);
        apply_triggers(g_ports[i]);
    }
}

void read(uint16_t* buttons, float* x, float* y, int port) {
    if (port < 0 || port >= kPorts || g_blocked.load(std::memory_order_relaxed)) {
        *buttons = 0;
        *x = 0.0f;
        *y = 0.0f;
        return;
    }
    if (g_wait_release[port].load(std::memory_order_relaxed)) {
        if (g_ports[port].buttons.load(std::memory_order_relaxed) != 0) {
            *buttons = 0;
            *x = 0.0f;
            *y = 0.0f;
            return;
        }
        g_wait_release[port].store(false, std::memory_order_relaxed);
    }
    *buttons = g_ports[port].buttons.load(std::memory_order_relaxed);
    *x = g_ports[port].stick_x.load(std::memory_order_relaxed);
    *y = g_ports[port].stick_y.load(std::memory_order_relaxed);
}

void set_mouse_play(bool playing) { g_mouse_play.store(playing, std::memory_order_relaxed); }

void set_blocked(bool blocked) {
    if (g_blocked.exchange(blocked, std::memory_order_relaxed) && !blocked) {
        for (std::atomic<bool>& wait : g_wait_release) wait.store(true, std::memory_order_relaxed);
    }
}

bool mouse_captured() { return g_mouse_buttons_captured.load(std::memory_order_relaxed); }

void take_mouse(float* dx, float* dy, int* zoom) {
    std::lock_guard lock(g_mouse_mutex);
    const bool blocked = g_blocked.load(std::memory_order_relaxed);
    *dx = blocked ? 0.0f : g_mouse_dx;
    *dy = blocked ? 0.0f : g_mouse_dy;
    *zoom = blocked ? 0 : g_mouse_zoom;
    g_mouse_dx = 0.0f;
    g_mouse_dy = 0.0f;
    g_mouse_zoom = 0;
}

bool right_stick(float* x, float* y, int port) {
    *x = 0.0f;
    *y = 0.0f;
    if (port < 0 || port >= kPorts || !g_ports[port].has_pad.load(std::memory_order_relaxed)) {
        return false;
    }
    if (g_blocked.load(std::memory_order_relaxed)) {
        return true;  // centred
    }
    *x = g_ports[port].right_x.load(std::memory_order_relaxed);
    *y = g_ports[port].right_y.load(std::memory_order_relaxed);
    return true;
}

bool connected(int port) { return port == 0 || (port > 0 && port < kPorts && g_ports[port].connected.load(std::memory_order_relaxed)); }

std::string controller_name(int port) {
    if (port < 0 || port >= kPorts) {
        return "";
    }
    std::lock_guard lock(g_pad_name_mutex);
    return g_ports[port].name;
}

std::string controller_model() {
    std::lock_guard lock(g_pad_name_mutex);
    return g_port1.model;
}

PadKind controller_kind() { return g_pad_kind.load(std::memory_order_relaxed); }

PadKind controller_kind(int port) {
    return port >= 0 && port < kPorts ? g_ports[port].kind.load(std::memory_order_relaxed) : PadKind::None;
}

Controls mapping(int port, int button) {
    if (port < 0 || port >= kPorts || button < 0 || button >= kMappedButtonCount) {
        return 0;
    }
    return g_mapping[port][button].load();
}

void set_mapping(int port, int button, Controls controls) {
    if (port < 0 || port >= kPorts || button < 0 || button >= kMappedButtonCount) {
        return;
    }
    controls &= ~kUnmappable;
    for (int i = 0; i < kMappedButtonCount; i++) {
        g_mapping[port][i].store(i == button ? controls : g_mapping[port][i].load() & ~controls);
    }
    const std::string text = mapping_text(port);
    std::printf("[conker] controller %d: %s is %s (%s)\n", port + 1, kMappedButtons[button].label,
                control_names(controls, port).c_str(), text.c_str());
    input_config::set(mapping_key(port), mapping_is_default(port) ? "" : text);
}

Controls default_mapping(int button) {
    return button >= 0 && button < kMappedButtonCount ? kDefaultMapping[button] : 0;
}

void reset_mapping(int port) {
    if (port < 0 || port >= kPorts) {
        return;
    }
    for (int i = 0; i < kMappedButtonCount; i++) {
        g_mapping[port][i].store(kDefaultMapping[i]);
    }
    std::printf("[conker] controller %d: buttons as they were\n", port + 1);
    input_config::set(mapping_key(port), "");
}

bool mapping_is_default(int port) {
    for (int i = 0; i < kMappedButtonCount; i++) {
        if (mapping(port, i) != kDefaultMapping[i]) {
            return false;
        }
    }
    return true;
}

std::string control_names(Controls controls, int port) {
    // A port without a pad names them as controller 1's pad would be, or an Xbox pad's.
    PadKind kind = controller_kind(port);
    if (kind == PadKind::None) {
        kind = controller_kind();
    }
    std::string names;
    for (int control = 0; control < kControlCount; control++) {
        if ((controls & bit(control)) != 0) {
            names += (names.empty() ? "" : " / ") + control_name(control, kind);
        }
    }
    return names.empty() ? "NONE" : names;
}

// PadControl numbers SDL's buttons from 1, then the controls after them.
static_assert(int(PadControl::South) == SDL_CONTROLLER_BUTTON_A + 1 && int(PadControl::Touchpad) == SDL_CONTROLLER_BUTTON_TOUCHPAD + 1);
static_assert(int(PadControl::LeftTrigger) == kLeftTrigger + 1 && int(PadControl::RightStickRight) == kRightStickRight + 1);

PadControl shown_control(int port, int button, PadKind kind) {
    const Controls controls = mapping(port, button);
    if (controls == 0) {
        return PadControl::None;
    }
    Controls shown = kDefaultMapping[button];
    if (kMappedButtons[button].n64 == kB) {
        shown = bit(kind == PadKind::PlayStation ? SDL_CONTROLLER_BUTTON_X : SDL_CONTROLLER_BUTTON_B);
    } else if (kMappedButtons[button].n64 == kZ) {
        shown = bit(kRightTrigger);
    }
    if ((controls & shown) == 0) {
        shown = controls;
    }
    for (int control = 0; control < kControlCount; control++) {
        if ((shown & bit(control)) != 0) {
            return PadControl(control + 1);
        }
    }
    return PadControl::None;
}

void start_listening(int port) {
    g_heard.store(0);
    g_listen_fresh.store(true);
    g_listen_port.store(port >= 0 && port < kPorts ? port : -1);
}

void stop_listening() {
    g_listen_port.store(-1);
    g_heard.store(0);
    g_listen_keys.store(false);
    g_heard_key.store(0);
}

void start_listening_keys() {
    g_heard_key.store(0);
    g_listen_keys_fresh.store(true);
    g_listen_keys.store(true);
}

int take_heard_key() { return g_heard_key.exchange(0); }

const char* key_row_label(int row) {
    if (row < 0 || row >= kKeyRows) return "";
    return row < kMappedButtonCount ? kMappedButtons[row].label : kKeyActions[row - kMappedButtonCount].label;
}

std::vector<int> key_mapping(int button) {
    if (button < 0 || button >= kKeyRows) {
        return {};
    }
    std::lock_guard lock(g_key_mutex);
    return g_key_mapping[button];
}

std::vector<int> default_key_mapping(int button) {
    if (button >= kMappedButtonCount && button < kKeyRows) {
        return kKeyActionDefaults[button - kMappedButtonCount];
    }
    std::vector<int> keys;
    for (const Key& k : kKeys) {
        if (button >= 0 && button < kMappedButtonCount && k.n64 == kMappedButtons[button].n64) keys.push_back(k.key);
    }
    return keys;
}

void set_key_mapping(int button, const std::vector<int>& keys) {
    if (button < 0 || button >= kKeyRows) {
        return;
    }
    std::vector<int> kept;
    for (int key : keys) {
        if (key_mappable(key) && std::find(kept.begin(), kept.end(), key) == kept.end()) kept.push_back(key);
    }
    {
        std::lock_guard lock(g_key_mutex);
        for (int i = 0; i < kKeyRows; i++) {
            std::erase_if(g_key_mapping[i], [&](int key) { return std::find(kept.begin(), kept.end(), key) != kept.end(); });
        }
        g_key_mapping[button] = kept;
    }
    const std::string text = key_mapping_text();
    std::printf("[conker] keyboard: %s is %s (%s)\n", key_row_label(button), key_names(kept).c_str(), text.c_str());
    input_config::set("keys", key_mapping_is_default() ? "" : text);
}

void reset_key_mapping() {
    {
        std::lock_guard lock(g_key_mutex);
        for (int i = 0; i < kKeyRows; i++) {
            g_key_mapping[i] = default_key_mapping(i);
        }
    }
    std::printf("[conker] keyboard: keys as they were\n");
    input_config::set("keys", "");
}

bool key_mapping_is_default() {
    for (int i = 0; i < kKeyRows; i++) {
        if (key_mapping(i) != default_key_mapping(i)) {
            return false;
        }
    }
    return true;
}

int shown_key(int button) {
    const std::vector<int> keys = key_mapping(button);
    if (keys.empty()) {
        return 0;
    }
    // The default pictures' keys: Space, E, Enter, C (crouch), Q, F, and I, K, J and L.
    const std::vector<int> defaults = default_key_mapping(button);
    const int shown = kMappedButtons[button].n64 == kZ ? SDL_SCANCODE_C : defaults.empty() ? 0 : defaults.front();
    return std::find(keys.begin(), keys.end(), shown) != keys.end() ? shown : keys.front();
}

std::string key_label(int scancode) {
    switch (scancode) {
        case SDL_SCANCODE_RETURN: return "ENTER";
        case SDL_SCANCODE_KP_ENTER: return "ENTER";
        case SDL_SCANCODE_SPACE: return "SPACE";
        case SDL_SCANCODE_BACKSPACE: return "BKSP";
        case SDL_SCANCODE_CAPSLOCK: return "CAPS";
        case SDL_SCANCODE_LCTRL: return "LCTRL";
        case SDL_SCANCODE_LSHIFT: return "LSHFT";
        case SDL_SCANCODE_RSHIFT: return "RSHFT";
        case SDL_SCANCODE_RCTRL: return "RCTRL";
        case SDL_SCANCODE_DELETE: return "DEL";
        case SDL_SCANCODE_INSERT: return "INS";
        case SDL_SCANCODE_PAGEUP: return "PGUP";
        case SDL_SCANCODE_PAGEDOWN: return "PGDN";
        case SDL_SCANCODE_UP: return "UP";
        case SDL_SCANCODE_DOWN: return "DOWN";
        case SDL_SCANCODE_LEFT: return "LEFT";
        case SDL_SCANCODE_RIGHT: return "RIGHT";
        case SDL_SCANCODE_NONUSBACKSLASH: return "\\";
        case SDL_SCANCODE_APPLICATION: return "MENU";
        case SDL_SCANCODE_PRINTSCREEN: return "PRTSC";
        case SDL_SCANCODE_SCROLLLOCK: return "SCRLK";
        case SDL_SCANCODE_NUMLOCKCLEAR: return "NUMLK";
        default: break;
    }
    // The rest by SDL's name, upper case: "Keypad 4" as KP4, others cut to five letters.
    std::string name = SDL_GetScancodeName(static_cast<SDL_Scancode>(scancode));
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::toupper(c); });
    if (name.rfind("KEYPAD ", 0) == 0) {
        name = "KP" + name.substr(7);
    }
    std::erase(name, ' ');
    if (name.empty()) {
        return "KEY" + std::to_string(scancode);
    }
    return name.substr(0, 5);
}

std::string key_names(const std::vector<int>& keys) {
    std::string names;
    for (int key : keys) {
        names += (names.empty() ? "" : " / ") + key_label(key);
    }
    return names.empty() ? "NONE" : names;
}

Controls take_heard() { return g_heard.exchange(0); }

void read_menu(uint16_t* buttons, float* x, float* y) {
    if (g_blocked.load(std::memory_order_relaxed)) {
        *buttons = 0;
        *x = 0.0f;
        *y = 0.0f;
        return;
    }
    *buttons = g_menu_buttons.load(std::memory_order_relaxed);
    *x = g_port1.stick_x.load(std::memory_order_relaxed);
    *y = g_port1.stick_y.load(std::memory_order_relaxed);
}

void read_menu_pad(uint16_t* buttons, float* x, float* y) {
    *buttons = g_menu_pad_buttons.load(std::memory_order_relaxed);
    *x = g_menu_pad_x.load(std::memory_order_relaxed);
    *y = g_menu_pad_y.load(std::memory_order_relaxed);
}

bool take_debug_menu_press() {
    return g_debug_menu_presses.exchange(0, std::memory_order_relaxed) > 0 && !g_blocked.load(std::memory_order_relaxed);
}

bool has_rumble(int port) { return port >= 0 && port < kPorts && g_ports[port].has_rumble.load(std::memory_order_relaxed); }

void set_rumble(int port, bool on) {
    if (port >= 0 && port < kPorts) {
        g_ports[port].rumble_on.store(on, std::memory_order_relaxed);
    }
}

bool has_haptics(int port) {
    return port >= 0 && port < kPorts && g_ports[port].has_haptics.load(std::memory_order_relaxed);
}

void play_haptic(const Haptic& haptic, int port) {
    if (!has_haptics(port)) {
        return;
    }
    std::lock_guard lock(g_haptics_mutex);
    std::vector<Haptic>& waiting = g_ports[port].haptics;
    if (waiting.size() < kMaxWaitingHaptics) {
        waiting.push_back(haptic);
    }
}

bool has_adaptive_triggers(int port) {
    return port >= 0 && port < kPorts && g_ports[port].adaptive_triggers.load(std::memory_order_relaxed);
}

void set_trigger_effect(const TriggerEffect& effect, int port) {
    if (port < 0 || port >= kPorts) {
        return;
    }
    std::lock_guard lock(g_trigger_mutex);
    g_ports[port].trigger_wanted = effect;
}

bool camera_swapped() { return g_camera_swapped.load(std::memory_order_relaxed); }

void set_camera_swapped(bool swapped) { g_camera_swapped.store(swapped, std::memory_order_relaxed); }

} // namespace conker::input
