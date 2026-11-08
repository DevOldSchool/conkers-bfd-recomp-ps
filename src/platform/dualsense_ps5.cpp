// A DualSense's adaptive triggers and haptics on the PS5.
//
// The console's SDL (PacBrew's) opens a pad per signed-in user with scePadOpen and sets it to the
// compatible vibration mode (2), where the two motor levels SDL rumbles are all the pad plays, and it
// answers trigger effects with "unsupported". Here the same pad is reached by its handle
// (scePadGetHandle, for the user SDL named the pad after):
// - triggers: scePadSetTriggerEffect, with the layout of libScePad's ScePadTriggerEffectParam as the
//   open-source duaLib (github.com/WujekFoliarz/duaLib) mirrors it;
// - haptics: the pad's vibration audio port (sceAudioOutOpen port type 10, 48 kHz 16-bit stereo, one
//   channel per grip's actuator) with the pad in its advanced vibration mode (1), as native titles play
//   them and as the PS5 RetroArch port's haptics did. A mixer thread per pad turns each shaped effect
//   and the rumble pak's rumble into that stream, as Core Haptics plays them on macOS
//   (src/platform/haptics_apple.mm): sharpness picks the pitch, a transient is a short tap.
// When the port does not open, the pad stays in compatible mode and its motors play everything, as before.
#include "platform/dualsense_ps5.hpp"
#include "enhancements/feel/feel.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" {
int32_t scePadGetHandle(int32_t user_id, int32_t type, int32_t index);
int32_t scePadSetTriggerEffect(int32_t handle, const void* param);
int32_t scePadSetVibrationMode(int32_t handle, int32_t mode);
int32_t scePadReadState(int32_t handle, void* data);
int32_t sceUserServiceGetLoginUserIdList(int32_t* user_ids);
int32_t sceUserServiceGetUserName(int32_t user_id, char* name, size_t size);
int32_t sceAudioOutInit(void);
int32_t sceAudioOutOpen(int32_t user_id, int32_t type, int32_t index, uint32_t length, uint32_t rate, uint32_t param);
int32_t sceAudioOutOutput(int32_t handle, const void* data);
int32_t sceAudioOutClose(int32_t handle);
}

namespace conker::input::ps5 {

namespace {

constexpr int kPorts = 4;
constexpr int kMaxUsers = 4;  // sceUserServiceGetLoginUserIdList's list
constexpr int32_t kPadStandard = 0;
constexpr int32_t kVibrationAdvanced = 1;
constexpr int32_t kVibrationCompatible = 2;
constexpr int32_t kVibrationPort = 10;
constexpr uint32_t kRate = 48000;
constexpr uint32_t kGrain = 256;           // frames per sceAudioOutOutput
constexpr uint32_t kS16Stereo = 1;
constexpr float kPeak = 32000.0f;          // full intensity, of 32767

// --- triggers: libScePad's parameter block (120 bytes) ---
enum TriggerMode : int32_t {
    kModeOff = 0,
    kModeFeedback = 1,
    kModeWeapon = 2,
    kModeVibration = 3,
    kModeMultiplePositionFeedback = 4,
    kModeSlopeFeedback = 5,
    kModeMultiplePositionVibration = 6,
};
struct TriggerCommand {
    int32_t mode;
    uint8_t padding[4];
    uint8_t data[48];
};
struct TriggerParam {
    uint8_t mask;  // 0x01 L2, 0x02 R2
    uint8_t padding[7];
    TriggerCommand command[2];  // L2, R2
};
static_assert(sizeof(TriggerParam) == 120);

// A strength or amplitude in [0, 1] as the pad's 1 to 8, or 0 for none (as input_sdl.cpp's encoder).
uint8_t level(float value) { return static_cast<uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 8.0f)); }

// A position in [0, 1] as one of the pad's ten zones along the pull.
int zone(float position) {
    return std::clamp(static_cast<int>(std::lround(position * kTriggerZones)), 0, kTriggerZones - 1);
}

// The command for the effect; `simple` uses the single-position modes, for a pad that refuses the others.
TriggerCommand command_for(const TriggerEffect& effect, bool simple) {
    TriggerCommand c{};
    switch (effect.mode) {
        case TriggerEffect::Mode::Feedback: {
            uint8_t strongest = 0;
            int first = -1;
            for (int i = 0; i < kTriggerZones; i++) {
                const uint8_t strength = level(effect.zones[i]);
                c.data[i] = strength;
                strongest = std::max(strongest, strength);
                if (strength > 0 && first < 0) first = i;
            }
            if (strongest == 0) {
                std::memset(c.data, 0, sizeof(c.data));
                break;
            }
            c.mode = kModeMultiplePositionFeedback;
            if (simple) {
                std::memset(c.data, 0, sizeof(c.data));
                c.mode = kModeFeedback;
                c.data[0] = static_cast<uint8_t>(first);
                c.data[1] = strongest;
            }
            break;
        }
        case TriggerEffect::Mode::Weapon: {
            // The wall runs from zone 2 to 7 at the earliest and gives way by zone 8.
            const int first = std::clamp(zone(effect.start), 2, 7);
            const int last = std::clamp(zone(effect.end), first + 1, 8);
            c.mode = kModeWeapon;
            c.data[0] = static_cast<uint8_t>(first);
            c.data[1] = static_cast<uint8_t>(last);
            c.data[2] = std::max<uint8_t>(level(effect.strength), 1);
            break;
        }
        case TriggerEffect::Mode::Vibration: {
            const uint8_t amplitude = level(effect.amplitude);
            if (amplitude == 0) break;
            const uint8_t hz = static_cast<uint8_t>(std::lround(1.0f + std::clamp(effect.frequency, 0.0f, 1.0f) * 254.0f));
            if (simple) {
                c.mode = kModeVibration;
                c.data[0] = static_cast<uint8_t>(zone(effect.start));
                c.data[1] = amplitude;
                c.data[2] = hz;
            } else {
                c.mode = kModeMultiplePositionVibration;
                c.data[0] = hz;
                for (int i = zone(effect.start); i < kTriggerZones; i++) c.data[1 + i] = amplitude;
            }
            break;
        }
        case TriggerEffect::Mode::Off:
            break;
    }
    return c;
}

// --- haptics: a voice per effect, mixed into the vibration port's stream ---
struct Voice {
    float intensity = 0.0f;
    float hz = 0.0f;
    uint32_t length = 0;  // frames; 0 while it is the rumble pak's (until stopped)
    uint32_t played = 0;
    bool left = true;
    bool right = true;
    bool transient = false;
    bool rumble = false;
    bool deep = false;  // low sharpness: the octave above added
    bool tuned_pitch = false;  // the rumble at the tuning's rumble_hz (it can change while it plays)
    double phase = 0.0;
};

struct Pad {
    int32_t user = -1;
    int32_t handle = -1;
    int32_t audio = -1;
    std::thread mixer;
    std::atomic<bool> stop{false};
    std::mutex mutex;  // voices
    std::vector<Voice> voices;
};

std::array<Pad, kPorts> g_pads;

// How the effects are shaped: the "ps5." lines of conker-recomp-data/feel.cfg (src/enhancements/feel),
// where the values tuned by hand on a console are built in. The mixer takes in a change within a
// second of the file being saved.
struct Tuning {
    float hz_deep = 38.0f;       // sharpness 0: a deep thud
    float hz_sharp = 250.0f;     // sharpness 1: a crisp click
    float curve = 0.6f;          // intensity^curve: Core Haptics' intensity is as felt, not as amplitude
    float punch = 6.0f;          // how far a sine is squared off toward a square (0: a pure sine)
    float tap_ms_deep = 110.0f;   // a tap's length at sharpness 0
    float tap_ms_sharp = 25.0f;  // and at 1
    float tap_hold = 0.5f;      // the part of a tap at full strength before it falls away
    float octave = 0.35f;        // the octave above, added to deep effects
    float deep_below = 0.35f;    // sharpness under which an effect is deep
    float tap_gain = 1.0f;       // overall strength of taps
    float buzz_gain = 1.6f;      // of continuous effects
    float rumble_gain = 1.3f;   // the rumble pak's tone
    float rumble_noise = 0.4f;   // and its roughness
    float rumble_hz = 50.0f;
    float deep_noise = 0.6f;    // the rumble's roughness in deep effects too: a THUD, not a hum
    float deep_stretch = 1.4f;   // deep continuous effects last this much longer (a gun round's thump)
    float tap_punch = 2.0f;      // punch for taps alone: a softer landing without a softer gun
    float tap_attack_ms = 5.0f;  // how quickly a tap comes in (1: a hard edge)
};

std::mutex g_tuning_mutex;
Tuning g_tuning;

struct TuningKey {
    const char* name;
    float Tuning::*field;
};
constexpr TuningKey kTuningKeys[] = {
    {"hz_deep", &Tuning::hz_deep}, {"hz_sharp", &Tuning::hz_sharp}, {"curve", &Tuning::curve},
    {"punch", &Tuning::punch}, {"tap_ms_deep", &Tuning::tap_ms_deep}, {"tap_ms_sharp", &Tuning::tap_ms_sharp},
    {"tap_hold", &Tuning::tap_hold}, {"octave", &Tuning::octave}, {"deep_below", &Tuning::deep_below},
    {"tap_gain", &Tuning::tap_gain}, {"buzz_gain", &Tuning::buzz_gain}, {"rumble_gain", &Tuning::rumble_gain},
    {"rumble_noise", &Tuning::rumble_noise}, {"rumble_hz", &Tuning::rumble_hz},
    {"deep_noise", &Tuning::deep_noise}, {"deep_stretch", &Tuning::deep_stretch},
    {"tap_punch", &Tuning::tap_punch}, {"tap_attack_ms", &Tuning::tap_attack_ms},
};

Tuning tuning() {
    std::lock_guard lock(g_tuning_mutex);
    return g_tuning;
}

// Takes in feel.cfg's "ps5." values when they changed. Mixer threads, about once a second each.
void reload_tuning() {
    static std::mutex reload_mutex;
    static uint32_t seen = UINT32_MAX;
    std::lock_guard reload(reload_mutex);
    conker::feel::refresh();
    if (conker::feel::generation() == seen) return;
    seen = conker::feel::generation();
    Tuning read;
    for (const TuningKey& key : kTuningKeys) {
        read.*key.field = conker::feel::number((std::string("ps5.") + key.name).c_str());
    }
    {
        std::lock_guard lock(g_tuning_mutex);
        g_tuning = read;
    }
    std::printf("[conker] haptics: PS5 shaping from feel.cfg:");
    for (const TuningKey& key : kTuningKeys) std::printf(" %s=%g", key.name, read.*key.field);
    std::printf("\n");
    std::fflush(stdout);
}

// Core Haptics' sharpness as a pitch: 0 a deep thud, 1 a crisp click.
float pitch(const Tuning& t, float sharpness) {
    return t.hz_deep + std::clamp(sharpness, 0.0f, 1.0f) * (t.hz_sharp - t.hz_deep);
}

float gain(const Tuning& t, float intensity) { return std::pow(std::clamp(intensity, 0.0f, 1.0f), t.curve); }

// A sine squared off toward a square: more push per cycle, which the hand reads as a harder hit.
float punchy(float punch, float wave) { return punch > 0.01f ? std::tanh(punch * wave) / std::tanh(punch) : wave; }

// A tap's length: a deep one is a thud, a sharp one a click.
float tap_seconds(const Tuning& t, float sharpness) {
    const float s = std::clamp(sharpness, 0.0f, 1.0f);
    return (t.tap_ms_deep + s * (t.tap_ms_sharp - t.tap_ms_deep)) / 1000.0f;
}

void mix(Pad& pad) {
    std::array<int16_t, kGrain * 2> out{};
    uint32_t noise = 0x9e3779b9u;
    float rough_left = 0.0f, rough_right = 0.0f;
    int failures = 0;
    uint32_t grains = 0;
    while (!pad.stop.load(std::memory_order_relaxed)) {
        if (grains++ % 188 == 0) {  // about once a second
            reload_tuning();
        }
        const Tuning t = tuning();
        {
        // Under the lock: a grain is a few microseconds' work, and a rumble stopped meanwhile must not
        // come back.
        std::lock_guard lock(pad.mutex);
        std::vector<Voice>& voices = pad.voices;
        for (uint32_t i = 0; i < kGrain; i++) {
            float left = 0.0f, right = 0.0f;
            // A little filtered noise under the rumble so it reads as a motor, not a pure tone.
            noise = noise * 1664525u + 1013904223u;
            rough_left += 0.08f * ((static_cast<float>((noise >> 9) & 0xFFFF) / 32768.0f - 1.0f) - rough_left);
            noise = noise * 1664525u + 1013904223u;
            rough_right += 0.08f * ((static_cast<float>((noise >> 9) & 0xFFFF) / 32768.0f - 1.0f) - rough_right);
            for (Voice& v : voices) {
                if (!v.rumble && v.played >= v.length) continue;
                float envelope = 1.0f;
                if (v.transient) {
                    // A tap: in at once, full for its first third, then falling away.
                    const float at = static_cast<float>(v.played) / static_cast<float>(v.length);
                    // The ease-in at most a quarter of the tap, so a short sharp tap still reaches its strength.
                    const float attack_frames = std::min(kRate * std::max(t.tap_attack_ms, 0.1f) / 1000.0f, 0.25f * v.length);
                    const float attack = std::min(1.0f, v.played / std::max(attack_frames, 1.0f));
                    const float hold = std::clamp(t.tap_hold, 0.0f, 0.95f);
                    const float fall = at < hold ? 1.0f : (1.0f - at) / (1.0f - hold);
                    envelope = attack * fall * fall * t.tap_gain;
                } else if (!v.rumble) {
                    // 3 ms in and out, so a continuous effect does not click.
                    const float edge = kRate * 0.003f;
                    envelope = std::min({1.0f, v.played / edge, (v.length - v.played) / edge}) * t.buzz_gain;
                }
                const double angle = 6.283185307179586 * v.phase;
                float wave = punchy(v.transient ? t.tap_punch : t.punch, static_cast<float>(std::sin(angle)));
                if (v.deep) {
                    // Some of the octave above, so a deep effect is felt as a thump and not only heard.
                    wave = (1.0f - 0.7f * t.octave) * wave + t.octave * static_cast<float>(std::sin(2.0 * angle));
                }
                v.phase += (v.tuned_pitch ? t.rumble_hz : v.hz) / kRate;
                if (v.phase >= 1.0) v.phase -= 1.0;
                const float base = v.intensity * envelope;
                // The pak's rumble near full scale, as strong as the motors it stands in for.
                // Deep effects get the rumble's roughness too, so they land as a THUD rather than a hum.
                const float rough = v.deep ? t.deep_noise : 0.0f;
                // A sharper rumble (the Uzis') keeps little of the deep one's roughness.
                const float rumble_rough = v.deep ? t.rumble_noise : 0.25f * t.rumble_noise;
                if (v.left) left += base * (v.rumble ? t.rumble_gain * wave + rumble_rough * rough_left : wave + rough * rough_left);
                if (v.right) right += base * (v.rumble ? t.rumble_gain * wave + rumble_rough * rough_right : wave + rough * rough_right);
                v.played++;
            }
            out[2 * i] = static_cast<int16_t>(std::clamp(left * kPeak, -32768.0f, 32767.0f));
            out[2 * i + 1] = static_cast<int16_t>(std::clamp(right * kPeak, -32768.0f, 32767.0f));
        }
        voices.erase(std::remove_if(voices.begin(), voices.end(),
                                    [](const Voice& v) { return !v.rumble && v.played >= v.length; }),
                     voices.end());
        }
        // sceAudioOutOutput waits for the port, which paces this thread.
        if (sceAudioOutOutput(pad.audio, out.data()) < 0) {
            if (++failures >= 40) {
                std::printf("[conker] haptics: vibration port stopped answering; the motors play instead\n");
                scePadSetVibrationMode(pad.handle, kVibrationCompatible);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        } else {
            failures = 0;
        }
    }
}

// The signed-in user SDL named a pad after (PacBrew's SDL names each pad with its user's name).
int32_t user_named(const std::string& name) {
    int32_t users[kMaxUsers];
    if (sceUserServiceGetLoginUserIdList(users) != 0) return -1;
    int32_t only = -1;
    int count = 0;
    for (int32_t user : users) {
        if (user == -1) continue;
        char user_name[64] = {};
        if (sceUserServiceGetUserName(user, user_name, sizeof(user_name)) == 0 && name == user_name) return user;
        only = user;
        count++;
    }
    return count == 1 ? only : -1;
}

// scePadReadState's data, as PacBrew's SDL declares it (PS5_PadData in SDL_ps5joystick.h), up to and
// including `connected`, with room for the rest.
struct PadState {
    uint32_t buttons;
    uint8_t sticks[4];
    uint8_t analog[2];
    uint16_t padding;
    float quat[4];
    float velocity[3];
    float acceleration[3];
    uint8_t touch[24];  // PS5_PadTouchData: count, padding, two touches of 8 bytes
    uint8_t connected;
    uint8_t rest[63];   // timestamp, ext, count, unknown
};
static_assert(offsetof(PadState, connected) == 76);

Pad* pad_of(int port) {
    if (port < 0 || port >= kPorts || g_pads[port].handle < 0) return nullptr;
    return &g_pads[port];
}

}  // namespace

bool open_dualsense(int port, const std::string& sdl_name) {
    if (port < 0 || port >= kPorts) return false;
    close_dualsense(port);
    Pad& pad = g_pads[port];
    pad.user = user_named(sdl_name);
    pad.handle = pad.user >= 0 ? scePadGetHandle(pad.user, kPadStandard, 0) : -1;
    if (pad.handle < 0) {
        std::printf("[conker] controller %d: no pad handle for its user (%d)\n", port + 1, pad.handle);
        pad.handle = -1;
        return false;
    }
    sceAudioOutInit();  // SDL's audio has done it already: harmless again
    pad.audio = sceAudioOutOpen(pad.user, kVibrationPort, 0, kGrain, kRate, kS16Stereo);
    const int32_t mode = scePadSetVibrationMode(pad.handle, pad.audio >= 0 ? kVibrationAdvanced : kVibrationCompatible);
    std::printf("[conker] controller %d: pad handle %d, vibration port %d, vibration mode %s (%d)\n", port + 1,
                pad.handle, pad.audio, pad.audio >= 0 ? "advanced" : "compatible", mode);
    if (pad.audio < 0) {
        pad.audio = -1;
        return false;
    }
    pad.stop.store(false);
    pad.mixer = std::thread(mix, std::ref(pad));
    return true;
}

void close_dualsense(int port) {
    if (port < 0 || port >= kPorts) return;
    Pad& pad = g_pads[port];
    if (pad.mixer.joinable()) {
        pad.stop.store(true);
        pad.mixer.join();
    }
    if (pad.audio >= 0) {
        sceAudioOutClose(pad.audio);
        pad.audio = -1;
    }
    {
        std::lock_guard lock(pad.mutex);
        pad.voices.clear();
    }
    pad.handle = -1;
    pad.user = -1;
}

bool pad_connected(const std::string& sdl_name) {
    const int32_t user = user_named(sdl_name);
    if (user < 0) return true;  // not known: as SDL lists it
    const int32_t handle = scePadGetHandle(user, kPadStandard, 0);  // SDL's scePadOpen made it
    if (handle < 0) return true;
    PadState state{};
    return scePadReadState(handle, &state) != 0 || state.connected != 0;
}

bool set_dualsense_triggers(int port, const TriggerEffect& effect) {
    Pad* pad = pad_of(port);
    if (pad == nullptr) return false;
    TriggerParam param{};
    param.mask = 0x01 | 0x02;  // both triggers are Z
    param.command[0] = param.command[1] = command_for(effect, false);
    int32_t result = scePadSetTriggerEffect(pad->handle, &param);
    if (result != 0 && (effect.mode == TriggerEffect::Mode::Feedback || effect.mode == TriggerEffect::Mode::Vibration)) {
        // Some pads refuse the multiple-position modes: the single-position ones then.
        param.command[0] = param.command[1] = command_for(effect, true);
        result = scePadSetTriggerEffect(pad->handle, &param);
    }
    static bool reported[kPorts] = {};
    if (result != 0 && !reported[port]) {
        reported[port] = true;
        std::printf("[conker] controller %d: scePadSetTriggerEffect failed (0x%08x)\n", port + 1, static_cast<unsigned>(result));
    }
    return result == 0;
}

bool play_dualsense_haptic(int port, const Haptic& haptic) {
    Pad* pad = pad_of(port);
    if (pad == nullptr || pad->audio < 0 || !pad->mixer.joinable()) return false;
    Voice v;
    const Tuning t = tuning();
    v.intensity = gain(t, haptic.intensity);
    v.transient = haptic.duration <= 0.0f;
    v.hz = pitch(t, haptic.sharpness);
    v.deep = haptic.sharpness < t.deep_below;
    const float seconds = v.transient ? tap_seconds(t, haptic.sharpness)
                                      : haptic.duration * (v.deep ? t.deep_stretch : 1.0f);
    v.length = std::max<uint32_t>(1, static_cast<uint32_t>(seconds * kRate));
    v.left = haptic.grip != Haptic::Grip::Right;
    v.right = haptic.grip != Haptic::Grip::Left;
    std::lock_guard lock(pad->mutex);
    pad->voices.push_back(v);
    return true;
}

bool set_dualsense_rumble(int port, bool on, const RumbleFeel& feel) {
    Pad* pad = pad_of(port);
    if (pad == nullptr || pad->audio < 0 || !pad->mixer.joinable()) return false;
    std::lock_guard lock(pad->mutex);
    pad->voices.erase(std::remove_if(pad->voices.begin(), pad->voices.end(), [](const Voice& v) { return v.rumble; }),
                      pad->voices.end());
    if (on) {
        Voice v;
        v.rumble = true;
        const Tuning t = tuning();
        v.intensity = gain(t, feel.intensity);
        // The deepest by default, as Core Haptics plays the pak (sharpness 0); sharper for, say, the Uzis.
        v.hz = feel.sharpness > 0.0f ? pitch(t, feel.sharpness) : t.rumble_hz;
        v.deep = feel.sharpness < t.deep_below;
        v.tuned_pitch = feel.sharpness <= 0.0f;
        pad->voices.push_back(v);
    }
    return true;
}

}  // namespace conker::input::ps5
