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
// CONKER_VIRTUAL_PAD=<file> attaches an SDL virtual game controller driven by a script, so the
// whole pad path (hot-plug, mapping, deadzone) can be exercised without hardware. Each line is
// "<vi> <length in VIs> <control>...", where a control is an SDL button name (a, b, x, start,
// leftshoulder, dpup, ...) or "<axis>=<value>" (leftx, lefty, rightx, righty: -1 to 1, SDL's +y is
// down; lefttrigger, righttrigger: 0 released to 1). '#' starts a comment. Real pads are then ignored.
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
    g_pad_virtual = SDL_JoystickIsVirtual(device_index) == SDL_TRUE;
    g_has_trigger_rumble = SDL_GameControllerHasRumbleTriggers(g_pad) == SDL_TRUE;
    g_trigger_rumble_applied = false;
    g_motor_effects.clear();
    g_haptics_via = HapticsVia::Motors;
#ifdef __APPLE__
    if (SDL_GameControllerGetType(g_pad) == SDL_CONTROLLER_TYPE_PS5 && !g_force_motors) {
        g_haptics_via = HapticsVia::CoreHaptics;
    }
#endif
    if (g_pad_virtual && !g_force_motors) {
        g_haptics_via = HapticsVia::Log;
    }
    g_has_haptics.store(g_has_rumble.load() || g_pad_virtual);
    static const char* const kVia[] = {"Core Haptics", "motors", "logged"};
    g_adaptive_triggers.store(SDL_GameControllerGetType(g_pad) == SDL_CONTROLLER_TYPE_PS5 || g_pad_virtual);
    g_trigger_stale = true;
    std::printf("[conker] controller 1: %s (%s%s%s%s%s)\n", SDL_GameControllerName(g_pad),
                g_has_rumble.load() ? "rumble pak" : "no rumble", g_has_haptics.load() ? ", haptics on " : "",
                g_has_haptics.load() ? kVia[static_cast<int>(g_haptics_via)] : "",
                g_has_trigger_rumble ? ", trigger motors" : "", g_adaptive_triggers.load() ? ", adaptive triggers" : "");
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

// The rumble pak's on and off print as such; haptic effects on the motors print their strengths.
int SDLCALL virtual_pad_rumble(void* /*userdata*/, Uint16 low, Uint16 high) {
    static Uint16 last_low = 0;
    static Uint16 last_high = 0;
    if (low == last_low && high == last_high) {
        return 0;
    }
    last_low = low;
    last_high = high;
    const unsigned long long vi = conker::current_vi();
    if (low == high && (low == 0 || low == 0xFFFF)) {
        std::printf("[conker] virtual pad: VI %llu: rumble %s\n", vi, low != 0 ? "on" : "off");
    } else {
        std::printf("[conker] virtual pad: VI %llu: rumble low %.2f high %.2f\n", vi, low / 65535.0f, high / 65535.0f);
    }
    return 0;
}

int SDLCALL virtual_pad_rumble_triggers(void* /*userdata*/, Uint16 left, Uint16 right) {
    static Uint16 last_left = 0;
    static Uint16 last_right = 0;
    if (left != last_left || right != last_right) {
        last_left = left;
        last_right = right;
        std::printf("[conker] virtual pad: VI %llu: trigger rumble left %.2f right %.2f\n",
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
    if (SDL_GameControllerSendEffect(g_pad, &effects, sizeof(effects)) == 0 || g_pad_virtual) {
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
    if (g_pad != nullptr && g_adaptive_triggers.load() && !(g_trigger_applied == TriggerEffect{})) {
        send_triggers(TriggerEffect{});
    }
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
    desc.RumbleTriggers = virtual_pad_rumble_triggers;
    desc.SendEffect = virtual_pad_effect;
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

// Sets the motors from the rumble pak and the haptic effects still playing, the strongest of them
// for each motor. The pak's motor is on or off and drives both grip motors at full strength. A
// motor set for an effect stops by itself when the next effect ends, so a late update cuts an
// effect short rather than holding it; the pak's is held kRumbleHoldMs, so a stalled game stops it.
// SDL only renews the expiry when the strength is unchanged, so this costs nothing per frame.
void apply_rumble(Uint64 now) {
    std::erase_if(g_motor_effects, [now](const MotorEffect& effect) { return effect.end_ms <= now; });
    if (g_pad == nullptr || !g_has_rumble.load(std::memory_order_relaxed)) {
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
    const bool pak = g_rumble_on.load(std::memory_order_relaxed);
    const Uint16 low_strength = pak ? kRumbleStrength : motor_strength(low);
    const Uint16 high_strength = pak ? kRumbleStrength : motor_strength(high);
    const Uint32 hold = pak ? kRumbleHoldMs : static_cast<Uint32>(next_end - now);
    const bool on = low_strength != 0 || high_strength != 0;
    int result = 0;
    if (on) {
        result = SDL_GameControllerRumble(g_pad, low_strength, high_strength, hold);
    } else if (g_rumble_applied) {
        result = SDL_GameControllerRumble(g_pad, 0, 0, 0);
    }
    g_rumble_applied = on;

    if (g_has_trigger_rumble) {
        const Uint16 left_strength = motor_strength(left_trigger);
        const Uint16 right_strength = motor_strength(right_trigger);
        const bool triggers_on = left_strength != 0 || right_strength != 0;
        int trigger_result = 0;
        if (triggers_on) {
            trigger_result = SDL_GameControllerRumbleTriggers(g_pad, left_strength, right_strength,
                                                              static_cast<Uint32>(next_trigger_end - now));
        } else if (g_trigger_rumble_applied) {
            trigger_result = SDL_GameControllerRumbleTriggers(g_pad, 0, 0, 0);
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
    open_first_pad();
    std::atexit(release_triggers);
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
                g_has_haptics.store(false);
                g_adaptive_triggers.store(false);
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
        // Z sits under the right hand on modern pads (R2/RT); the left trigger stays Z as well. A
        // trigger effect on adaptive triggers can move how far they are pulled for it.
        float z_at = kTriggerZ;
        if (g_adaptive_triggers.load(std::memory_order_relaxed)) {
            std::lock_guard lock(g_trigger_mutex);
            z_at = g_trigger_wanted.z_at;
        }
        const auto threshold = static_cast<Sint16>(z_at * 32767.0f);
        const bool left_trigger = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > threshold;
        const bool right_trigger = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > threshold;
        if (left_trigger || right_trigger) buttons |= kZ;
        if (left_trigger && !g_left_trigger_held) g_trigger_grip = Haptic::Grip::Left;
        if (right_trigger && !g_right_trigger_held) g_trigger_grip = Haptic::Grip::Right;
        g_left_trigger_held = left_trigger;
        g_right_trigger_held = right_trigger;

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

    std::vector<Haptic> haptics;
    {
        std::lock_guard lock(g_haptics_mutex);
        haptics.swap(g_haptics);
    }
    const Uint64 now = SDL_GetTicks64();
    if (g_pad != nullptr && g_has_haptics.load(std::memory_order_relaxed)) {
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

    if (g_pad != nullptr && g_adaptive_triggers.load(std::memory_order_relaxed)) {
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

bool has_haptics() { return g_has_haptics.load(std::memory_order_relaxed); }

void play_haptic(const Haptic& haptic) {
    if (!has_haptics()) {
        return;
    }
    std::lock_guard lock(g_haptics_mutex);
    if (g_haptics.size() < kMaxWaitingHaptics) {
        g_haptics.push_back(haptic);
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
