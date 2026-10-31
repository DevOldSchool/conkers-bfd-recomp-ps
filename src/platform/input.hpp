// Player input for the four controller ports. The RT64 build reads SDL game controllers, one per
// port in the order they connect, and the keyboard on controller 1 (src/platform/input_sdl.cpp,
// pumped by the window's event loop); the null build has no input device
// (src/platform/input_headless.cpp). Every port's pad has its own rumble pak, haptics and adaptive
// triggers below; controller 1 alone has the keyboard, mouse, button pictures and debug menu button.
#pragma once

#include <cstdint>
#include <string>

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
bool mouse_captured();
void take_mouse(float* dx, float* dy, int* zoom);

// Whether a controller is plugged into a port: controller 1 always is (the keyboard), the others
// while a pad holds them. Safe to call from any thread.
bool connected(int port);

// A port's pad's name, or "" when none is connected (controller 1 on the keyboard only). Safe to
// call from any thread.
std::string controller_name(int port = 0);

// The kind of pad controller 1 is, for its button pictures (src/enhancements/pad_icons): None for
// the keyboard alone, Other for pads that are neither Xbox nor PlayStation ones. Safe to call from
// any thread.
enum class PadKind { None, Xbox, PlayStation, Other };
PadKind controller_kind();

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

// Whether C-left and C-right are swapped, on the right stick and on J/L, so that pushing left turns
// the view left; part of consistent controls (src/enhancements/consistent_controls), which set it.
// Safe to call from any thread.
bool camera_swapped();
void set_camera_swapped(bool swapped);

} // namespace conker::input
