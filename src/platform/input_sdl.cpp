// Controller 1 from an SDL game controller and the keyboard. update() samples both on the main
// thread after the window pumps events; the game thread reads the result through read() when the
// game calls osContGetReadData. The two sources are combined: buttons are OR-ed, and the pad's
// stick wins over the keyboard's whenever it is outside the deadzone.
//
// Pad (SDL's Xbox-style names): south A, west and east B, LT Z, LB L, RT and RB R, Start Start,
// D-pad D-pad, left stick the analog stick, right stick the C buttons. Keyboard: see kKeys and
// the README.
// Back (Create/Share on a PlayStation pad, View on an Xbox pad) and the ` key open the debug menu
// (src/enhancements/debug_menu); they are not N64 buttons.
//
// A pad that can rumble (SDL_GameControllerHasRumble; a DualSense, DualShock 4, Xbox or Switch
// Pro pad) counts as a rumble pak in controller 1, and the game's motor on/off drives both of its
// motors at full strength, like the pak's single motor. Rumble on Bluetooth PlayStation pads needs
// SDL's extended reports, which init() turns on; on macOS those pads go through Apple's
// GameController framework instead, which rumbles without them.
//
// CONKER_VIRTUAL_PAD=<file> attaches an SDL virtual game controller driven by a script, so the
// whole pad path (hot-plug, mapping, deadzone) can be exercised without hardware. Each line is
// "<vi> <length in VIs> <control>...", where a control is an SDL button name (a, b, x, start,
// leftshoulder, dpup, ...) or "<axis>=<value>" (leftx, lefty, rightx, righty: -1 to 1, SDL's +y is
// down; lefttrigger, righttrigger: 0 released to 1). '#' starts a comment. Real pads are then ignored.
// The virtual pad can rumble and logs each change.
#include "platform/input.hpp"
#include "platform/input_sdl.hpp"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "runtime/game.hpp"

namespace conker::input {

namespace {

constexpr float kStickDeadzone = 0.20f;        // radial, of full deflection
constexpr Sint16 kCButtonThreshold = 16384;    // right stick, half deflection
constexpr Sint16 kTriggerThreshold = 10000;    // LT/RT, about a third of the travel
constexpr Uint16 kRumbleStrength = 0xFFFF;     // both motors; the pak's motor is on or off
constexpr Uint32 kRumbleHoldMs = 250;          // renewed every update, so a stalled game stops it

struct PadButton {
    SDL_GameControllerButton button;
    uint16_t n64;
};

constexpr PadButton kPadButtons[] = {
    {SDL_CONTROLLER_BUTTON_A, kA},
    {SDL_CONTROLLER_BUTTON_X, kB},
    {SDL_CONTROLLER_BUTTON_B, kB},
    {SDL_CONTROLLER_BUTTON_START, kStart},
    {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, kL},
    {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, kR},
    {SDL_CONTROLLER_BUTTON_DPAD_UP, kDUp},
    {SDL_CONTROLLER_BUTTON_DPAD_DOWN, kDDown},
    {SDL_CONTROLLER_BUTTON_DPAD_LEFT, kDLeft},
    {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, kDRight},
};

struct Key {
    SDL_Scancode key;
    uint16_t n64;
};

constexpr Key kKeys[] = {
    {SDL_SCANCODE_SPACE, kA},
    {SDL_SCANCODE_LSHIFT, kB},
    {SDL_SCANCODE_Q, kZ},
    {SDL_SCANCODE_RETURN, kStart},
    {SDL_SCANCODE_KP_ENTER, kStart},
    {SDL_SCANCODE_TAB, kL},
    {SDL_SCANCODE_E, kR},
    {SDL_SCANCODE_I, kCUp},
    {SDL_SCANCODE_K, kCDown},
    {SDL_SCANCODE_J, kCRight},  // swapped like the right stick: J turns the view left
    {SDL_SCANCODE_L, kCLeft},
    {SDL_SCANCODE_T, kDUp},
    {SDL_SCANCODE_G, kDDown},
    {SDL_SCANCODE_F, kDLeft},
    {SDL_SCANCODE_H, kDRight},
};

SDL_GameController* g_pad = nullptr;
SDL_JoystickID g_pad_id = -1;

bool g_scripted = false;  // CONKER_VIRTUAL_PAD: only the virtual pad counts

std::atomic<uint16_t> g_buttons{0};
std::atomic<float> g_stick_x{0.0f};
std::atomic<float> g_stick_y{0.0f};
std::atomic<int> g_debug_menu_presses{0};
std::mutex g_pad_name_mutex;
std::string g_pad_name;                  // controller 1's, for controller_name()
bool g_debug_menu_held = false;          // main thread only

std::atomic<bool> g_has_rumble{false};  // the open pad can rumble
std::atomic<PadKind> g_pad_kind{PadKind::None};
std::atomic<bool> g_rumble_on{false};   // the game's motor state, applied by update()
std::atomic<bool> g_camera_swapped{true};  // C-left and C-right (camera_swapped)
bool g_rumble_applied = false;          // main thread only

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

void open_pad(int device_index) {
    if (g_pad != nullptr || !SDL_IsGameController(device_index)) {
        return;
    }
    // A real pad would take controller 1 from the script, and SDL hands pad input to every
    // process, so a scripted run would also pick up whatever is played in another window.
    if (g_scripted && !SDL_JoystickIsVirtual(device_index)) {
        return;
    }
    g_pad = SDL_GameControllerOpen(device_index);
    if (g_pad == nullptr) {
        std::fprintf(stderr, "[conker] could not open controller %d: %s\n", device_index, SDL_GetError());
        return;
    }
    g_pad_id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_pad));
    {
        std::lock_guard lock(g_pad_name_mutex);
        g_pad_name = SDL_GameControllerName(g_pad) ? SDL_GameControllerName(g_pad) : "controller";
    }
    g_has_rumble.store(SDL_GameControllerHasRumble(g_pad) == SDL_TRUE);
    g_pad_kind.store(pad_kind(g_pad));
    g_rumble_applied = false;
    std::printf("[conker] controller 1: %s (%s)\n", SDL_GameControllerName(g_pad),
                g_has_rumble.load() ? "rumble pak" : "no rumble");
    // CONKER_RUMBLE_TEST=1: one second of rumble on connecting, to check the pad without the game.
    const char* test = std::getenv("CONKER_RUMBLE_TEST");
    if (g_has_rumble.load() && test != nullptr && test[0] == '1') {
        const int result = SDL_GameControllerRumble(g_pad, kRumbleStrength, kRumbleStrength, 1000);
        std::printf("[conker] rumble test: %s\n", result == 0 ? "1 s at full strength" : SDL_GetError());
    }
}

void open_first_pad() {
    for (int i = 0; i < SDL_NumJoysticks() && g_pad == nullptr; i++) {
        open_pad(i);
    }
}

// --- scripted virtual pad (CONKER_VIRTUAL_PAD) ---
struct ScriptStep {
    uint64_t vi;
    uint64_t length;
    std::string text;
    std::vector<SDL_GameControllerButton> buttons;
    std::vector<std::pair<SDL_GameControllerAxis, float>> axes;
    bool reported;
};

std::vector<ScriptStep> g_script;
SDL_Joystick* g_virtual = nullptr;

bool load_script(const char* path) {
    std::ifstream file(path);
    if (!file) {
        std::fprintf(stderr, "[conker] CONKER_VIRTUAL_PAD: cannot open %s\n", path);
        return false;
    }
    std::string line;
    for (int number = 1; std::getline(file, line); number++) {
        line = line.substr(0, line.find('#'));
        std::istringstream in(line);
        ScriptStep step{};
        if (!(in >> step.vi >> step.length)) {
            continue;
        }
        step.text = line;
        for (std::string control; in >> control;) {
            const size_t eq = control.find('=');
            if (eq == std::string::npos) {
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
        g_script.push_back(std::move(step));
    }
    return true;
}

int SDLCALL virtual_pad_rumble(void* /*userdata*/, Uint16 low, Uint16 high) {
    static bool last = false;
    const bool on = low != 0 || high != 0;
    if (on != last) {
        last = on;
        std::printf("[conker] virtual pad: VI %llu: rumble %s\n", static_cast<unsigned long long>(conker::current_vi()), on ? "on" : "off");
    }
    return 0;
}

void attach_virtual_pad() {
    const char* path = std::getenv("CONKER_VIRTUAL_PAD");
    if (path == nullptr || *path == '\0' || !load_script(path)) {
        return;
    }
    SDL_VirtualJoystickDesc desc{};
    desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    desc.naxes = SDL_CONTROLLER_AXIS_MAX;
    desc.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
    desc.button_mask = (1u << SDL_CONTROLLER_BUTTON_MAX) - 1;
    desc.axis_mask = (1u << SDL_CONTROLLER_AXIS_MAX) - 1;
    desc.name = "Conker virtual pad";
    desc.Rumble = virtual_pad_rumble;
    const int index = SDL_JoystickAttachVirtualEx(&desc);
    g_virtual = index < 0 ? nullptr : SDL_JoystickOpen(index);
    if (g_virtual == nullptr) {
        std::fprintf(stderr, "[conker] CONKER_VIRTUAL_PAD: %s\n", SDL_GetError());
        return;
    }
    g_scripted = true;
    std::printf("[conker] virtual pad: %zu scripted steps from %s\n", g_script.size(), path);
}

// Takes effect when SDL next updates joysticks, i.e. on the next event pump.
void drive_virtual_pad() {
    if (g_virtual == nullptr) {
        return;
    }
    const uint64_t now = conker::current_vi();
    bool buttons[SDL_CONTROLLER_BUTTON_MAX] = {};
    float axes[SDL_CONTROLLER_AXIS_MAX] = {};
    for (ScriptStep& step : g_script) {
        if (now < step.vi || now >= step.vi + step.length) {
            continue;
        }
        if (!step.reported) {
            step.reported = true;
            std::printf("[conker] virtual pad: VI %llu:%s\n", static_cast<unsigned long long>(now), step.text.c_str());
        }
        for (SDL_GameControllerButton b : step.buttons) buttons[b] = true;
        for (const auto& [a, v] : step.axes) axes[a] = v;
    }
    for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; b++) {
        SDL_JoystickSetVirtualButton(g_virtual, b, buttons[b] ? SDL_PRESSED : SDL_RELEASED);
    }
    for (int a = 0; a < SDL_CONTROLLER_AXIS_MAX; a++) {
        // A trigger's raw axis runs from -32768 (released) to 32767, which SDL maps to 0..32767; a raw
        // 0 would be half pressed, which is Z.
        const bool trigger = a == SDL_CONTROLLER_AXIS_TRIGGERLEFT || a == SDL_CONTROLLER_AXIS_TRIGGERRIGHT;
        const float v = std::fmax(trigger ? 0.0f : -1.0f, std::fmin(axes[a], 1.0f));
        const float raw = trigger ? v * 65535.0f - 32768.0f : v * 32767.0f;
        SDL_JoystickSetVirtualAxis(g_virtual, a, static_cast<Sint16>(raw));
    }
}

float axis(SDL_GameControllerAxis which) {
    return static_cast<float>(SDL_GameControllerGetAxis(g_pad, which)) / 32767.0f;
}

} // namespace

void init() {
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
    open_first_pad();
}

void handle_event(const SDL_Event& event) {
    switch (event.type) {
        case SDL_CONTROLLERDEVICEADDED:
            open_pad(event.cdevice.which);
            break;
        case SDL_CONTROLLERDEVICEREMOVED:
            if (g_pad != nullptr && event.cdevice.which == g_pad_id) {
                std::printf("[conker] controller 1 disconnected\n");
                SDL_GameControllerClose(g_pad);
                g_pad = nullptr;
                {
                    std::lock_guard lock(g_pad_name_mutex);
                    g_pad_name.clear();
                }
                g_pad_id = -1;
                g_has_rumble.store(false);
                g_pad_kind.store(PadKind::None);
                open_first_pad();
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
    bool debug_menu = false;

    // Keys held with Cmd or Alt are shortcuts (Cmd+Enter toggles full screen), not game input.
    if ((SDL_GetModState() & (KMOD_GUI | KMOD_ALT)) == 0) {
        int key_count = 0;
        const Uint8* keys = SDL_GetKeyboardState(&key_count);
        for (const Key& k : kKeys) {
            if (k.key < key_count && keys[k.key]) {
                buttons |= k.n64;
            }
        }
        x = static_cast<float>((keys[SDL_SCANCODE_D] || keys[SDL_SCANCODE_RIGHT]) - (keys[SDL_SCANCODE_A] || keys[SDL_SCANCODE_LEFT]));
        y = static_cast<float>((keys[SDL_SCANCODE_W] || keys[SDL_SCANCODE_UP]) - (keys[SDL_SCANCODE_S] || keys[SDL_SCANCODE_DOWN]));
        debug_menu = keys[SDL_SCANCODE_GRAVE] != 0;
    }

    if (g_pad != nullptr) {
        for (const PadButton& b : kPadButtons) {
            if (SDL_GameControllerGetButton(g_pad, b.button)) {
                buttons |= b.n64;
            }
        }
        debug_menu = debug_menu || SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_BACK);
        // Z sits under the right hand on modern pads (R2/RT); the left trigger stays Z as well.
        if (SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > kTriggerThreshold) buttons |= kZ;
        if (SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > kTriggerThreshold) buttons |= kZ;

        const Sint16 cx = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_RIGHTX);
        const Sint16 cy = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_RIGHTY);
        // Conker turns the camera the opposite way to the C button pressed (C-left swings it to the
        // right), so the horizontal axis is swapped: pushing the stick left turns the view left.
        if (cx > kCButtonThreshold) buttons |= kCLeft;
        if (cx < -kCButtonThreshold) buttons |= kCRight;
        if (cy > kCButtonThreshold) buttons |= kCDown;
        if (cy < -kCButtonThreshold) buttons |= kCUp;

        // Radial deadzone, rescaled so full deflection still reaches the edge. SDL's +y is down.
        const float px = axis(SDL_CONTROLLER_AXIS_LEFTX);
        const float py = -axis(SDL_CONTROLLER_AXIS_LEFTY);
        const float magnitude = std::sqrt(px * px + py * py);
        if (magnitude > kStickDeadzone) {
            const float scale = std::fmin((magnitude - kStickDeadzone) / (1.0f - kStickDeadzone), 1.0f) / magnitude;
            x = px * scale;
            y = py * scale;
        }
    }

    if (!g_camera_swapped.load(std::memory_order_relaxed)) {
        const uint16_t c = buttons & (kCLeft | kCRight);
        buttons = static_cast<uint16_t>((buttons & ~(kCLeft | kCRight)) | ((c & kCLeft) ? kCRight : 0) |
                                        ((c & kCRight) ? kCLeft : 0));
    }
    g_buttons.store(buttons, std::memory_order_relaxed);
    if (debug_menu && !g_debug_menu_held) {
        g_debug_menu_presses.fetch_add(1, std::memory_order_relaxed);
    }
    g_debug_menu_held = debug_menu;
    g_stick_x.store(x, std::memory_order_relaxed);
    g_stick_y.store(y, std::memory_order_relaxed);

    // SDL only renews the expiry when the strength is unchanged, so this costs nothing per frame.
    if (g_pad != nullptr && g_has_rumble.load(std::memory_order_relaxed)) {
        const bool on = g_rumble_on.load(std::memory_order_relaxed);
        int result = 0;
        if (on) {
            result = SDL_GameControllerRumble(g_pad, kRumbleStrength, kRumbleStrength, kRumbleHoldMs);
        } else if (g_rumble_applied) {
            result = SDL_GameControllerRumble(g_pad, 0, 0, 0);
        }
        static bool reported = false;
        if (result != 0 && !reported) {
            reported = true;
            std::fprintf(stderr, "[conker] controller 1 rumble failed: %s\n", SDL_GetError());
        }
        g_rumble_applied = on;
    }
}

void read(uint16_t* buttons, float* x, float* y) {
    *buttons = g_buttons.load(std::memory_order_relaxed);
    *x = g_stick_x.load(std::memory_order_relaxed);
    *y = g_stick_y.load(std::memory_order_relaxed);
}

std::string controller_name() {
    std::lock_guard lock(g_pad_name_mutex);
    return g_pad_name;
}

PadKind controller_kind() { return g_pad_kind.load(std::memory_order_relaxed); }

bool take_debug_menu_press() { return g_debug_menu_presses.exchange(0, std::memory_order_relaxed) > 0; }

bool has_rumble() { return g_has_rumble.load(std::memory_order_relaxed); }

void set_rumble(bool on) { g_rumble_on.store(on, std::memory_order_relaxed); }

bool camera_swapped() { return g_camera_swapped.load(std::memory_order_relaxed); }

void set_camera_swapped(bool swapped) { g_camera_swapped.store(swapped, std::memory_order_relaxed); }

} // namespace conker::input
