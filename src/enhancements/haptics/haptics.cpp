// Haptics on a DualSense driven by the game's sound effects. Every sound effect starts in
// sndp_play_sound(player, sound, volume, flags), which the [[patches.hook]] in config/us.toml calls
// this from: chosen sounds play a haptic effect on the pad (src/platform/input.hpp). The volume is
// no guide to strength, since each sound has its own (Conker's reload click plays at 9233, his
// footsteps at 32767), so each effect has its own. The game's rumble pak still rumbles as before.
// CONKER_HAPTICS=0 turns them off.
//
// The sounds so far were picked out with the log below in scene 6 (Windy's slingshot B pad), by
// when they start.
//
// CONKER_SOUND_LOG=1 prints each sound as it starts, with Conker's animation (the u16 at +0x84 of
// his actor, *D_80088720), to find the ids of new ones.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "recomp.h"
#include "enhancements/status.hpp"
#include "enhancements/switches.hpp"
#include "platform/input.hpp"
#include "runtime/game.hpp"

namespace {

inline gpr kseg0(uint32_t address) { return static_cast<gpr>(static_cast<int32_t>(address)); }

constexpr uint32_t kConker = 0x80088720;    // D_80088720: Conker's actor
constexpr uint32_t kActorAnimation = 0x84;  // u16

using conker::input::Haptic;
using Grip = Haptic::Grip;

// The effect of the sounds first to last (variants of one sound are numbered together).
struct SoundHaptic {
    uint16_t first;
    uint16_t last;
    Haptic haptic;
};

constexpr Haptic tap(float intensity, float sharpness, Grip grip) { return {intensity, sharpness, 0.0f, grip}; }
constexpr Haptic buzz(float intensity, float sharpness, float duration, Grip grip) {
    return {intensity, sharpness, duration, grip};
}

constexpr SoundHaptic kSounds[] = {
    // Slingshot: taken out with B in the right hand, then fired with either trigger, in whose
    // grip the shot is felt.
    {1598, 1599, tap(0.5f, 0.6f, Grip::Right)},         // taken out from a B pad, put away
    {1678, 1680, tap(1.0f, 0.9f, Grip::Trigger)},       // a shot (one of three, at random)
    {54, 54, buzz(0.35f, 0.2f, 0.25f, Grip::Trigger)},  // the elastic stretched again
    {17, 17, tap(0.4f, 1.0f, Grip::Trigger)},           // reloaded
    // Conker landing from a jump or a fall. Footsteps (588-599 on stone) were tried and felt too
    // strong for something so frequent.
    {8, 8, tap(0.8f, 0.2f, Grip::Both)},
};

std::atomic<bool> g_enabled{[] {
    const char* value = std::getenv("CONKER_HAPTICS");
    return value == nullptr || value[0] != '0';
}()};  // the debug menu switches it

bool haptics_enabled() { return g_enabled.load(std::memory_order_relaxed); }

const bool g_log = [] {
    const char* value = std::getenv("CONKER_SOUND_LOG");
    return value != nullptr && value[0] == '1';
}();

uint16_t conker_animation(uint8_t* rdram) {
    const uint32_t conker = static_cast<uint32_t>(MEM_W(0, kseg0(kConker)));
    if (conker < 0x80000000u || conker >= 0x80800000u) {
        return 0;
    }
    return MEM_HU(kActorAnimation, kseg0(conker));
}

}  // namespace

// At sndp_play_sound's entry, on the game thread or the audio thread: $a1 is the sound (1 and
// up), $a2 its volume.
extern "C" void conker_sound_started(uint8_t* rdram, recomp_context* ctx) {
    const uint16_t sound = static_cast<uint16_t>(ctx->r5);
    const uint16_t volume = static_cast<uint16_t>(ctx->r6);
    if (g_log) {
        std::printf("[conker] VI %llu: sound %u volume %u (Conker animation 0x%X)\n",
                    static_cast<unsigned long long>(conker::current_vi()), sound, volume, conker_animation(rdram));
    }
    if (!haptics_enabled() || !conker::input::has_haptics()) {
        return;
    }
    const auto it = std::find_if(std::begin(kSounds), std::end(kSounds), [sound](const SoundHaptic& entry) {
        return sound >= entry.first && sound <= entry.last;
    });
    if (it == std::end(kSounds)) {
        return;
    }
    conker::input::play_haptic(it->haptic);
}

nlohmann::json conker::status::haptics() {
    return enhancement("haptics", "DualSense haptics from sound effects", haptics_enabled(), "CONKER_HAPTICS",
                       {{"pad_has_haptics", conker::input::has_haptics()}});
}

conker::switches::Switch conker::switches::haptics() {
    return {"HAPTICS", haptics_enabled, [](bool on) { g_enabled.store(on); }};
}
