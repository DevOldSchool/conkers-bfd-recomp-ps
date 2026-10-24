// The four controller ports from SDL game controllers, and controller 1 from the keyboard too.
// update() samples them on the main thread after the window pumps events; the game thread reads
// the result through read() when the game polls the controllers. A pad takes the lowest free port
// when it connects and frees it when it disconnects; the others keep theirs. Controller 1 combines
// its pad and the keyboard: buttons are OR-ed, and the pad's stick wins over the keyboard's
// whenever it is outside the deadzone.
//
// Pad (SDL's Xbox-style names): south A, west and east B, LT Z, LB L, RT and RB R, Start Start,
// D-pad D-pad, left stick the analog stick, right stick the C buttons. Keyboard: see kKeys and
// the README.
// Back (Create/Share on a PlayStation pad, View on an Xbox pad) and the ` key open the debug menu
// (src/enhancements/debug_menu); they are not N64 buttons.
//
// A pad that can rumble (SDL_GameControllerHasRumble; a DualSense, DualShock 4, Xbox or Switch
// Pro pad) counts as a rumble pak in its port, and the game's motor on/off drives both of its
// motors at full strength, like the pak's single motor. Haptic effects, Core Haptics and adaptive
// triggers below are controller 1's only. Rumble on Bluetooth PlayStation pads needs
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
// A DualSense's adaptive triggers act as set_trigger_effect asks, both triggers alike since
// both are Z. SDL sends the effect where its own driver reads the pad (HIDAPI); on macOS, where
// Apple's framework reads it, src/platform/adaptive_triggers_apple.mm sets them instead.
//
// CONKER_VIRTUAL_PAD=<file>[,<file>...] attaches an SDL virtual game controller per file, driven by
// its script, so the whole pad path (hot-plug, ports, mapping, deadzone) can be exercised without
// hardware. Each line is "<vi> <length in VIs> <control>...", where a control is an SDL button
// name (a, b, x, start, leftshoulder, dpup, ...) or "<axis>=<value>" (leftx, lefty, rightx, righty:
// -1 to 1, SDL's +y is down; lefttrigger, righttrigger: 0 released to 1), or "plug <vi>" /
// "unplug <vi>". '#' starts a comment. Real pads are then ignored.
// The virtual pad can rumble and logs each change, logs each haptic effect (or, with
// CONKER_HAPTICS_MOTORS=1, the motor strengths it plays as), and has adaptive triggers, logging
// each effect sent to them.
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

// A controller port and the pad holding it. Controller 1's pad also has the haptics, adaptive
// triggers and button pictures below; it plays the keyboard alongside, and has a port without a pad.
struct Port {
    SDL_GameController* pad = nullptr;  // main thread only
    SDL_JoystickID id = -1;             // main thread only
    bool rumble_applied = false;        // main thread only
    std::atomic<bool> connected{false};
    std::atomic<uint16_t> buttons{0};
    std::atomic<float> stick_x{0.0f};
    std::atomic<float> stick_y{0.0f};
    std::atomic<bool> has_rumble{false};  // the pad can rumble
    std::atomic<bool> rumble_on{false};   // the game's motor state, applied by update()
};
Port g_ports[kPorts];
Port& g_port1 = g_ports[0];

bool g_scripted = false;  // CONKER_VIRTUAL_PAD: only the virtual pads count

std::atomic<int> g_debug_menu_presses{0};
std::mutex g_pad_name_mutex;
std::string g_pad_name;                  // controller 1's, for controller_name()
bool g_debug_menu_held = false;          // main thread only

std::atomic<PadKind> g_pad_kind{PadKind::None};
std::atomic<bool> g_camera_swapped{true};  // C-left and C-right (camera_swapped)
bool g_pak_rumble_applied = false;      // main thread only: the pak's rumble playing through Core Haptics

std::atomic<bool> g_has_haptics{false};  // the open pad plays shaped effects

// How the open pad plays haptic effects (main thread only).
enum class HapticsVia { CoreHaptics, Motors, Log };
HapticsVia g_haptics_via = HapticsVia::Motors;
bool g_has_trigger_rumble = false;       // main thread only: SDL_GameControllerHasRumbleTriggers
bool g_trigger_rumble_applied = false;   // main thread only

// An effect playing on the rumble motors, each strength in [0, 1], until end_ms (SDL_GetTicks64).
struct MotorEffect {
    Uint64 end_ms;
    float low;
    float high;
    float left_trigger;
    float right_trigger;
};
std::vector<MotorEffect> g_motor_effects;  // main thread only
constexpr float kSharpTap = 0.04f;         // s: a tap on the motors at sharpness 1, which spin up fast
constexpr float kSoftTap = 0.09f;          // s: at sharpness 0, a thump the heavy motor needs longer for

const bool g_force_motors = [] {
    const char* value = std::getenv("CONKER_HAPTICS_MOTORS");
    return value != nullptr && value[0] == '1';
}();
std::mutex g_haptics_mutex;
std::vector<Haptic> g_haptics;           // waiting for update(), under g_haptics_mutex
std::vector<Haptic> g_port_haptics[kPorts];          // controllers 2 to 4's, likewise
std::vector<MotorEffect> g_port_motor_effects[kPorts];  // main thread only: controllers 2 to 4's
constexpr size_t kMaxWaitingHaptics = 8;
Haptic::Grip g_trigger_grip = Haptic::Grip::Right;  // main thread only: Grip::Trigger's side
bool g_left_trigger_held = false;                   // main thread only
bool g_right_trigger_held = false;                  // main thread only

std::atomic<bool> g_adaptive_triggers{false};  // the open pad has them
std::mutex g_trigger_mutex;
TriggerEffect g_trigger_wanted;                // set_trigger_effect's, under g_trigger_mutex
TriggerEffect g_trigger_applied;               // main thread only
bool g_trigger_stale = true;                   // main thread only: the pad has not had g_trigger_applied
bool g_pad_virtual = false;                    // main thread only: the open pad is CONKER_VIRTUAL_PAD's

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

// CONKER_RUMBLE_TEST=1: one second of rumble on connecting, to check the pad without the game.
void rumble_test(const Port& port) {
    const char* test = std::getenv("CONKER_RUMBLE_TEST");
    if (port.has_rumble.load() && test != nullptr && test[0] == '1') {
        const int result = SDL_GameControllerRumble(port.pad, kRumbleStrength, kRumbleStrength, 1000);
        std::printf("[conker] rumble test: %s\n", result == 0 ? "1 s at full strength" : SDL_GetError());
    }
}

// Controller 1's pad: its name and picture kind, haptics and adaptive triggers.
void open_port1_extras(int device_index) {
    {
        std::lock_guard lock(g_pad_name_mutex);
        g_pad_name = SDL_GameControllerName(g_port1.pad) ? SDL_GameControllerName(g_port1.pad) : "controller";
    }
    g_pad_kind.store(pad_kind(g_port1.pad));
#ifdef __APPLE__
    if (g_pak_rumble_applied) {
        apple::set_dualsense_rumble(false);
    }
#endif
    g_pak_rumble_applied = false;
    g_pad_virtual = SDL_JoystickIsVirtual(device_index) == SDL_TRUE;
    g_has_trigger_rumble = SDL_GameControllerHasRumbleTriggers(g_port1.pad) == SDL_TRUE;
    g_trigger_rumble_applied = false;
    g_motor_effects.clear();
    g_haptics_via = HapticsVia::Motors;
#ifdef __APPLE__
    if (SDL_GameControllerGetType(g_port1.pad) == SDL_CONTROLLER_TYPE_PS5 && !g_force_motors) {
        g_haptics_via = HapticsVia::CoreHaptics;
    }
#endif
    if (g_pad_virtual && !g_force_motors) {
        g_haptics_via = HapticsVia::Log;
    }
    g_has_haptics.store(g_port1.has_rumble.load() || g_pad_virtual);
    static const char* const kVia[] = {"Core Haptics", "motors", "logged"};
    g_adaptive_triggers.store(SDL_GameControllerGetType(g_port1.pad) == SDL_CONTROLLER_TYPE_PS5 || g_pad_virtual);
    g_trigger_stale = true;
    std::printf("[conker] controller 1: %s (%s%s%s%s%s)\n", SDL_GameControllerName(g_port1.pad),
                g_port1.has_rumble.load() ? "rumble pak" : "no rumble", g_has_haptics.load() ? ", haptics on " : "",
                g_has_haptics.load() ? kVia[static_cast<int>(g_haptics_via)] : "",
                g_has_trigger_rumble ? ", trigger motors" : "", g_adaptive_triggers.load() ? ", adaptive triggers" : "");
}

void close_port1_extras() {
    {
        std::lock_guard lock(g_pad_name_mutex);
        g_pad_name.clear();
    }
    g_has_haptics.store(false);
    g_adaptive_triggers.store(false);
    g_pad_kind.store(PadKind::None);
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
    port.has_rumble.store(SDL_GameControllerHasRumble(port.pad) == SDL_TRUE);
    port.rumble_applied = false;
    port.buttons.store(0);
    port.stick_x.store(0.0f);
    port.stick_y.store(0.0f);
    if (number == 1) {
        open_port1_extras(device_index);
    } else {
        std::printf("[conker] controller %d: %s (%s)\n", number, SDL_GameControllerName(port.pad),
                    port.has_rumble.load() ? "rumble pak" : "no rumble");
    }
    port.connected.store(true);
    rumble_test(port);
}

void open_all_pads() {
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        open_pad(i);
    }
}

void close_pad(Port& port) {
    const int number = static_cast<int>(&port - g_ports) + 1;
    std::printf("[conker] controller %d disconnected\n", number);
    port.connected.store(false);
    port.has_rumble.store(false);
    port.buttons.store(0);
    port.stick_x.store(0.0f);
    port.stick_y.store(0.0f);
    SDL_GameControllerClose(port.pad);
    port.pad = nullptr;
    port.id = -1;
    if (number == 1) {
        close_port1_extras();
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
        pad.script.push_back(std::move(step));
    }
    return true;
}

// The rumble pak's on and off print as such; haptic effects on the motors print their strengths.
// "virtual pad" for the only one, "virtual pad 2" and so on when there are several.
std::string virtual_pad_name(const VirtualPad& pad) {
    return g_virtual_pads.size() == 1 ? "virtual pad" : "virtual pad " + std::to_string(pad.number);
}

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

int SDLCALL virtual_pad_effect(void* /*userdata*/, const void* data, int size) {
    if (size < static_cast<int>(sizeof(DualSenseEffects))) {
        return -1;
    }
    const auto* effects = static_cast<const DualSenseEffects*>(data);
    std::printf("[conker] virtual pad: VI %llu: triggers", static_cast<unsigned long long>(conker::current_vi()));
    for (Uint8 byte : effects->right_trigger) std::printf(" %02x", byte);
    std::printf("\n");
    return 0;
}

// Sends the effect to the open pad: SDL's own DualSense driver takes the effect report, and on
// macOS a pad read through Apple's framework is set there.
bool send_triggers(const TriggerEffect& trigger) {
    DualSenseEffects effects{};
    effects.enable_bits1 = 0x04 | 0x08;
    encode_trigger(trigger, effects.right_trigger);
    encode_trigger(trigger, effects.left_trigger);
    // sdl2-compat reports a virtual pad's effect as failed even when the pad takes it.
    if (SDL_GameControllerSendEffect(g_port1.pad, &effects, sizeof(effects)) == 0 || g_pad_virtual) {
        return true;
    }
#ifdef __APPLE__
    return apple::set_dualsense_triggers(trigger);
#else
    return false;
#endif
}

// Leaves the triggers free when the game quits: a DualSense keeps an effect until told otherwise.
void release_triggers() {
    if (g_port1.pad != nullptr && g_adaptive_triggers.load() && !(g_trigger_applied == TriggerEffect{})) {
        send_triggers(TriggerEffect{});
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
        for (ScriptStep& step : pad.script) {
            if (now < step.vi || now >= step.vi + step.length) {
                continue;
            }
            if (!step.reported) {
                step.reported = true;
                std::printf("[conker] %s: VI %llu:%s\n", virtual_pad_name(pad).c_str(), static_cast<unsigned long long>(now),
                            step.text.c_str());
            }
            for (SDL_GameControllerButton b : step.buttons) buttons[b] = true;
            for (const auto& [a, v] : step.axes) axes[a] = v;
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

// Adds a pad's buttons to `buttons`, and its stick to x and y when outside the deadzone. A trigger
// counts as Z once pulled past z_at; left_trigger and right_trigger say which are.
void read_pad(SDL_GameController* pad, float z_at, uint16_t& buttons, float& x, float& y, bool& left_trigger,
              bool& right_trigger) {
    for (const PadButton& b : kPadButtons) {
        if (SDL_GameControllerGetButton(pad, b.button)) {
            buttons |= b.n64;
        }
    }
    // Z sits under the right hand on modern pads (R2/RT); the left trigger stays Z as well.
    const auto threshold = static_cast<Sint16>(z_at * 32767.0f);
    left_trigger = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > threshold;
    right_trigger = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > threshold;
    if (left_trigger || right_trigger) buttons |= kZ;

    const Sint16 cx = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTX);
    const Sint16 cy = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTY);
    // Conker turns the camera the opposite way to the C button pressed (C-left swings it to the
    // right), so the horizontal axis is swapped: pushing the stick left turns the view left.
    if (cx > kCButtonThreshold) buttons |= kCLeft;
    if (cx < -kCButtonThreshold) buttons |= kCRight;
    if (cy > kCButtonThreshold) buttons |= kCDown;
    if (cy < -kCButtonThreshold) buttons |= kCUp;

    // Radial deadzone, rescaled so full deflection still reaches the edge. SDL's +y is down.
    const float px = axis(pad, SDL_CONTROLLER_AXIS_LEFTX);
    const float py = -axis(pad, SDL_CONTROLLER_AXIS_LEFTY);
    const float magnitude = std::sqrt(px * px + py * py);
    if (magnitude > kStickDeadzone) {
        const float scale = std::fmin((magnitude - kStickDeadzone) / (1.0f - kStickDeadzone), 1.0f) / magnitude;
        x = px * scale;
        y = py * scale;
    }
}

// C-left and C-right back the N64's way round when camera_swapped is off.
uint16_t unswap_camera(uint16_t buttons) {
    if (g_camera_swapped.load(std::memory_order_relaxed)) {
        return buttons;
    }
    const uint16_t c = buttons & (kCLeft | kCRight);
    return static_cast<uint16_t>((buttons & ~(kCLeft | kCRight)) | ((c & kCLeft) ? kCRight : 0) | ((c & kCRight) ? kCLeft : 0));
}

void store(Port& port, uint16_t buttons, float x, float y) {
    port.buttons.store(unswap_camera(buttons), std::memory_order_relaxed);
    port.stick_x.store(x, std::memory_order_relaxed);
    port.stick_y.store(y, std::memory_order_relaxed);
}

// --- haptic effects on the rumble motors ---
// A tap lasts long enough for the motor to spin up, longer the softer it is. A Grip::Both effect
// leans on the light motor the sharper it is and on the heavy one the softer: both run at full
// strength at sharpness 0.5. `on_trigger` is a Grip::Trigger effect, resolved to haptic.grip's side.
MotorEffect motor_effect(const Haptic& haptic, bool on_trigger, Uint64 now) {
    const float sharpness = std::clamp(haptic.sharpness, 0.0f, 1.0f);
    const float intensity = std::clamp(haptic.intensity, 0.0f, 1.0f);
    const float seconds = haptic.duration > 0.0f ? haptic.duration : kSoftTap + (kSharpTap - kSoftTap) * sharpness;
    MotorEffect effect{now + static_cast<Uint64>(seconds * 1000.0f + 0.5f), 0.0f, 0.0f, 0.0f, 0.0f};
    if (on_trigger && g_has_trigger_rumble) {
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

// Controllers 2 to 4: the rumble pak's motor on both grip motors at full strength, held
// kRumbleHoldMs so a stalled game stops it, and otherwise the strongest haptic effect still playing
// on each motor, until the next one ends (as apply_rumble does for controller 1).
void apply_pak_rumble(int index, Uint64 now) {
    Port& port = g_ports[index];
    std::vector<MotorEffect>& effects = g_port_motor_effects[index];
    std::erase_if(effects, [now](const MotorEffect& effect) { return effect.end_ms <= now; });
    if (port.pad == nullptr || !port.has_rumble.load(std::memory_order_relaxed)) {
        effects.clear();
        return;
    }
    float low = 0.0f;
    float high = 0.0f;
    Uint64 next_end = now + kRumbleHoldMs;
    for (const MotorEffect& effect : effects) {
        low = std::fmax(low, effect.low);
        high = std::fmax(high, effect.high);
        next_end = std::min(next_end, effect.end_ms);
    }
    const bool pak = port.rumble_on.load(std::memory_order_relaxed);
    const Uint16 low_strength = pak ? kRumbleStrength : motor_strength(low);
    const Uint16 high_strength = pak ? kRumbleStrength : motor_strength(high);
    const bool on = low_strength != 0 || high_strength != 0;
    if (on) {
        SDL_GameControllerRumble(port.pad, low_strength, high_strength, pak ? kRumbleHoldMs : static_cast<Uint32>(next_end - now));
    } else if (port.rumble_applied) {
        SDL_GameControllerRumble(port.pad, 0, 0, 0);
    }
    port.rumble_applied = on;
}

// Sets the motors from the rumble pak and the haptic effects still playing, the strongest of them
// for each motor. The pak's motor is on or off and drives both grip motors at full strength (on a
// DualSense through Core Haptics, its own rumble in both grips instead). A
// motor set for an effect stops by itself when the next effect ends, so a late update cuts an
// effect short rather than holding it; the pak's is held kRumbleHoldMs, so a stalled game stops it.
// SDL only renews the expiry when the strength is unchanged, so this costs nothing per frame.
void apply_rumble(Uint64 now) {
    std::erase_if(g_motor_effects, [now](const MotorEffect& effect) { return effect.end_ms <= now; });
    if (g_port1.pad == nullptr || !g_port1.has_rumble.load(std::memory_order_relaxed)) {
        g_motor_effects.clear();
        return;
    }
    float low = 0.0f;
    float high = 0.0f;
    float left_trigger = 0.0f;
    float right_trigger = 0.0f;
    Uint64 next_end = now + kRumbleHoldMs;
    Uint64 next_trigger_end = now + kRumbleHoldMs;
    for (const MotorEffect& effect : g_motor_effects) {
        low = std::fmax(low, effect.low);
        high = std::fmax(high, effect.high);
        left_trigger = std::fmax(left_trigger, effect.left_trigger);
        right_trigger = std::fmax(right_trigger, effect.right_trigger);
        if (effect.low > 0.0f || effect.high > 0.0f) next_end = std::min(next_end, effect.end_ms);
        if (effect.left_trigger > 0.0f || effect.right_trigger > 0.0f) next_trigger_end = std::min(next_trigger_end, effect.end_ms);
    }
    bool pak = g_port1.rumble_on.load(std::memory_order_relaxed);
#ifdef __APPLE__
    // A DualSense played through Core Haptics takes the pak as a deep rumble in both grips: SDL's
    // own rumble on it is a thin buzz.
    if (g_haptics_via == HapticsVia::CoreHaptics) {
        if (pak != g_pak_rumble_applied && apple::set_dualsense_rumble(pak)) {
            g_pak_rumble_applied = pak;
        }
        if (g_pak_rumble_applied == pak) {
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
        result = SDL_GameControllerRumble(g_port1.pad, low_strength, high_strength, hold);
    } else if (g_port1.rumble_applied) {
        result = SDL_GameControllerRumble(g_port1.pad, 0, 0, 0);
    }
    g_port1.rumble_applied = on;

    if (g_has_trigger_rumble) {
        const Uint16 left_strength = motor_strength(left_trigger);
        const Uint16 right_strength = motor_strength(right_trigger);
        const bool triggers_on = left_strength != 0 || right_strength != 0;
        int trigger_result = 0;
        if (triggers_on) {
            trigger_result = SDL_GameControllerRumbleTriggers(g_port1.pad, left_strength, right_strength,
                                                              static_cast<Uint32>(next_trigger_end - now));
        } else if (g_trigger_rumble_applied) {
            trigger_result = SDL_GameControllerRumbleTriggers(g_port1.pad, 0, 0, 0);
        }
        g_trigger_rumble_applied = triggers_on;
        result = result != 0 ? result : trigger_result;
    }
    static bool reported = false;
    if (result != 0 && !reported) {
        reported = true;
        std::fprintf(stderr, "[conker] controller 1 rumble failed: %s\n", SDL_GetError());
    }
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
    open_all_pads();
    std::atexit(release_triggers);
}

void handle_event(const SDL_Event& event) {
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

    if (g_port1.pad != nullptr) {
        debug_menu = debug_menu || SDL_GameControllerGetButton(g_port1.pad, SDL_CONTROLLER_BUTTON_BACK);
        // A trigger effect on adaptive triggers can move how far they are pulled for Z.
        float z_at = kTriggerZ;
        if (g_adaptive_triggers.load(std::memory_order_relaxed)) {
            std::lock_guard lock(g_trigger_mutex);
            z_at = g_trigger_wanted.z_at;
        }
        bool left_trigger = false;
        bool right_trigger = false;
        read_pad(g_port1.pad, z_at, buttons, x, y, left_trigger, right_trigger);
        if (left_trigger && !g_left_trigger_held) g_trigger_grip = Haptic::Grip::Left;
        if (right_trigger && !g_right_trigger_held) g_trigger_grip = Haptic::Grip::Right;
        g_left_trigger_held = left_trigger;
        g_right_trigger_held = right_trigger;
    }
    store(g_port1, buttons, x, y);
    if (debug_menu && !g_debug_menu_held) {
        g_debug_menu_presses.fetch_add(1, std::memory_order_relaxed);
    }
    g_debug_menu_held = debug_menu;

    std::vector<Haptic> port_haptics[kPorts];
    {
        std::lock_guard lock(g_haptics_mutex);
        for (int i = 1; i < kPorts; i++) port_haptics[i].swap(g_port_haptics[i]);
    }
    const Uint64 port_now = SDL_GetTicks64();
    for (int i = 1; i < kPorts; i++) {
        Port& port = g_ports[i];
        if (port.pad != nullptr && port.has_rumble.load(std::memory_order_relaxed)) {
            for (const Haptic& haptic : port_haptics[i]) {
                g_port_motor_effects[i].push_back(motor_effect(haptic, false, port_now));
            }
        }
        if (port.pad != nullptr) {
            uint16_t port_buttons = 0;
            float port_x = 0.0f;
            float port_y = 0.0f;
            bool left_trigger = false;
            bool right_trigger = false;
            read_pad(port.pad, kTriggerZ, port_buttons, port_x, port_y, left_trigger, right_trigger);
            store(port, port_buttons, port_x, port_y);
        }
        apply_pak_rumble(i, port_now);
    }

    std::vector<Haptic> haptics;
    {
        std::lock_guard lock(g_haptics_mutex);
        haptics.swap(g_haptics);
    }
    const Uint64 now = SDL_GetTicks64();
    if (g_port1.pad != nullptr && g_has_haptics.load(std::memory_order_relaxed)) {
        for (Haptic haptic : haptics) {
            const bool on_trigger = haptic.grip == Haptic::Grip::Trigger;
            if (on_trigger) {
                haptic.grip = g_trigger_grip;
            }
            if (g_haptics_via == HapticsVia::Log) {
                static const char* const kGrips[] = {"left", "right", "both"};
                std::printf("[conker] virtual pad: VI %llu: haptic %.2f sharpness %.2f for %.2f s, %s\n",
                            static_cast<unsigned long long>(conker::current_vi()), haptic.intensity, haptic.sharpness,
                            haptic.duration, kGrips[static_cast<int>(haptic.grip)]);
                continue;
            }
#ifdef __APPLE__
            // No DualSense among the GameController framework's pads (SDL_JOYSTICK_HIDAPI_PS5=1 reads
            // it through SDL's own driver): the motors, which SDL rumbles.
            if (g_haptics_via == HapticsVia::CoreHaptics && apple::play_dualsense_haptic(haptic)) {
                continue;
            }
#endif
            g_motor_effects.push_back(motor_effect(haptic, on_trigger, now));
        }
    }
    apply_rumble(now);

    if (g_port1.pad != nullptr && g_adaptive_triggers.load(std::memory_order_relaxed)) {
        TriggerEffect wanted;
        {
            std::lock_guard lock(g_trigger_mutex);
            wanted = g_trigger_wanted;
        }
        if (g_trigger_stale || !(wanted == g_trigger_applied)) {
            static bool reported = false;
            if (!send_triggers(wanted) && !reported) {
                reported = true;
                std::fprintf(stderr, "[conker] controller 1 adaptive triggers failed: %s\n", SDL_GetError());
            }
            g_trigger_applied = wanted;
            g_trigger_stale = false;
        }
    }
}

void read(uint16_t* buttons, float* x, float* y, int port) {
    if (port < 0 || port >= kPorts) {
        *buttons = 0;
        *x = 0.0f;
        *y = 0.0f;
        return;
    }
    *buttons = g_ports[port].buttons.load(std::memory_order_relaxed);
    *x = g_ports[port].stick_x.load(std::memory_order_relaxed);
    *y = g_ports[port].stick_y.load(std::memory_order_relaxed);
}

bool connected(int port) { return port == 0 || (port > 0 && port < kPorts && g_ports[port].connected.load(std::memory_order_relaxed)); }

std::string controller_name() {
    std::lock_guard lock(g_pad_name_mutex);
    return g_pad_name;
}

PadKind controller_kind() { return g_pad_kind.load(std::memory_order_relaxed); }

bool take_debug_menu_press() { return g_debug_menu_presses.exchange(0, std::memory_order_relaxed) > 0; }

bool has_rumble(int port) { return port >= 0 && port < kPorts && g_ports[port].has_rumble.load(std::memory_order_relaxed); }

void set_rumble(int port, bool on) {
    if (port >= 0 && port < kPorts) {
        g_ports[port].rumble_on.store(on, std::memory_order_relaxed);
    }
}

bool has_haptics(int port) {
    return port == 0 ? g_has_haptics.load(std::memory_order_relaxed) : has_rumble(port);
}

void play_haptic(const Haptic& haptic, int port) {
    if (!has_haptics(port)) {
        return;
    }
    std::lock_guard lock(g_haptics_mutex);
    std::vector<Haptic>& waiting = port == 0 ? g_haptics : g_port_haptics[port];
    if (waiting.size() < kMaxWaitingHaptics) {
        waiting.push_back(haptic);
    }
}

bool has_adaptive_triggers() { return g_adaptive_triggers.load(std::memory_order_relaxed); }

void set_trigger_effect(const TriggerEffect& effect) {
    std::lock_guard lock(g_trigger_mutex);
    g_trigger_wanted = effect;
}

bool camera_swapped() { return g_camera_swapped.load(std::memory_order_relaxed); }

void set_camera_swapped(bool swapped) { g_camera_swapped.store(swapped, std::memory_order_relaxed); }

} // namespace conker::input
