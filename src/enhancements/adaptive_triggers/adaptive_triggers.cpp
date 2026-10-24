// The mounted machine gun on the pad while Conker sits at it: a DualSense's triggers (both are Z)
// pull heavy, and the pad rumbles in both grips, as the gun is held with both hands: a rumble that
// grows as Z winds the barrels up, then a thump for every round over the rumble of the spinning
// barrels. The triggers only resist; the shaking is in the grips. Native helper called once a frame
// from the [[patches.hook]] in config/us.toml (func_1501878C); src/platform/input.hpp sends the
// trigger effect and plays the rumble (Core Haptics on a DualSense on macOS, the motors elsewhere).
// CONKER_ADAPTIVE_TRIGGERS=0 turns both off.
//
// The gun's state is in the player's block at +0x31C of Conker's actor (*D_80088720), the one
// func_150BDB70 reads to aim a mounted gun at where the player looks. Found in the wasps' hive
// (scene 39, debug report scene39-20261024-144343) by dumping RAM before, during and after
// holding Z:
// - u8 +0x197 is 2 while he is mounted on the gun (0 on foot, with the slingshot too);
// - u8 +0x199 is 1 while Z is held on it;
// - u16 +0x19E counts the spin-up down from 150, a step a frame, and the gun fires at 0. It stays
//   0 for a while after Z is let go, so a quick second press fires at once;
// - s16 +0x1AA counts the rounds fired, about one every 10-14 VIs; the barrels go on firing for
//   about half a second after Z is let go.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>

#include "recomp.h"

#include "platform/input.hpp"
#include "runtime/game.hpp"
#include "enhancements/status.hpp"
#include "enhancements/switches.hpp"

namespace {

inline gpr kseg0(uint32_t address) { return static_cast<gpr>(static_cast<int32_t>(address)); }

constexpr uint32_t kConker = 0x80088720;       // D_80088720: Conker's actor
constexpr uint32_t kWorldPaused = 0x800BEAC0;  // D_800BEAC0: the world frame is skipped (pause, menus)
constexpr uint32_t kActorModel = 0x4;          // u8: 0 for Conker
constexpr uint32_t kActorPlayer = 0x31C;       // the player's block

constexpr uint32_t kPlayerMounted = 0x197;     // u8
constexpr uint32_t kPlayerGunTrigger = 0x199;  // u8
constexpr uint32_t kPlayerGunSpinUp = 0x19E;   // u16
constexpr uint32_t kPlayerGunRounds = 0x1AA;   // s16

constexpr uint8_t kMountedGun = 2;
constexpr float kSpinUpFrames = 150.0f;

using conker::input::TriggerEffect;

constexpr TriggerEffect feedback(float from, float strength) {
    TriggerEffect effect;
    effect.mode = TriggerEffect::Mode::Feedback;
    for (int i = 0; i < conker::input::kTriggerZones; i++) {
        effect.zones[i] = static_cast<float>(i) >= from * conker::input::kTriggerZones ? strength : 0.0f;
    }
    return effect;
}

// Seated at the gun: free for the first fifth of the pull, then heavy all the way down.
constexpr TriggerEffect kGunReady = feedback(0.2f, 0.4f);

using conker::input::Haptic;

// The rumble, in both grips. Sharpness 0.25 is a deep rumble on a DualSense and still runs both
// motors of a pad that has two (lower would leave the light right one still).
constexpr float kRumbleSharpness = 0.25f;
constexpr Haptic rumble(float intensity, float seconds) {
    return {intensity, kRumbleSharpness, seconds, Haptic::Grip::Both};
}

// Rumbles overlap, renewed every kRumbleVis, so they feel like one that changes as it goes.
constexpr uint64_t kRumbleVis = 6;
constexpr float kRumbleSeconds = 0.15f;

// Winding up: grows from soft to strong as the barrels near full speed.
Haptic gun_spinning(float progress) { return rumble(0.2f + 0.4f * progress, kRumbleSeconds); }

// Firing: a hard thump as each round goes off, over the spinning barrels' steady rumble.
constexpr Haptic kGunRound = {1.0f, 0.3f, 0.08f, Haptic::Grip::Both};
constexpr Haptic kGunBarrels = rumble(0.45f, kRumbleSeconds);
constexpr uint64_t kStillFiringVis = 20;  // a round within this long: the barrels are still firing

std::atomic<bool> g_enabled{[] {
    const char* value = std::getenv("CONKER_ADAPTIVE_TRIGGERS");
    return value == nullptr || value[0] != '0';
}()};  // the debug menu switches it

bool adaptive_triggers_enabled() { return g_enabled.load(std::memory_order_relaxed); }

// The player's block while Conker is in the running world, or 0.
uint32_t player_block(uint8_t* rdram) {
    if (MEM_BU(0, kseg0(kWorldPaused)) != 0) {
        return 0;
    }
    const uint32_t conker = static_cast<uint32_t>(MEM_W(0, kseg0(kConker)));
    if (conker < 0x80000000u || conker >= 0x80800000u || MEM_BU(kActorModel, kseg0(conker)) != 0) {
        return 0;
    }
    const uint32_t player = static_cast<uint32_t>(MEM_W(kActorPlayer, kseg0(conker)));
    return player >= 0x80000000u && player < 0x80800000u ? player : 0;
}

// Game thread only.
int16_t g_last_rounds = 0;
uint64_t g_round_vi = 0;   // when the last round went off
uint64_t g_rumble_vi = 0;  // when the last steady rumble started
bool g_was_mounted = false;

void play(const Haptic& haptic) {
    if (conker::input::has_haptics()) {
        conker::input::play_haptic(haptic);
    }
}

// The rumble for this frame while Conker sits at the gun.
void gun_rumble(uint8_t* rdram, uint32_t player) {
    const int16_t rounds = static_cast<int16_t>(MEM_HU(kPlayerGunRounds, kseg0(player)));
    const uint64_t vi = conker::current_vi();
    if (g_was_mounted && rounds != g_last_rounds) {
        g_round_vi = vi;
        play(kGunRound);
    }
    g_last_rounds = rounds;
    g_was_mounted = true;

    if (g_rumble_vi != 0 && vi - g_rumble_vi < kRumbleVis) {
        return;
    }
    const uint16_t spin_up = MEM_HU(kPlayerGunSpinUp, kseg0(player));
    if (g_round_vi != 0 && vi - g_round_vi < kStillFiringVis) {
        play(kGunBarrels);
    } else if (MEM_BU(kPlayerGunTrigger, kseg0(player)) != 0 && spin_up != 0) {
        play(gun_spinning(1.0f - std::min(static_cast<float>(spin_up), kSpinUpFrames) / kSpinUpFrames));
    } else {
        return;
    }
    g_rumble_vi = vi;
}

} // namespace

// Once a frame, from func_1501878C just before it decides whether to run the world frame.
extern "C" void conker_adaptive_triggers(uint8_t* rdram, recomp_context* /*ctx*/) {
    const uint32_t player = player_block(rdram);
    const bool mounted = player != 0 && MEM_BU(kPlayerMounted, kseg0(player)) == kMountedGun;
    if (mounted && adaptive_triggers_enabled()) {
        gun_rumble(rdram, player);
    } else {
        g_was_mounted = false;
        g_round_vi = 0;
        g_rumble_vi = 0;
    }
    conker::input::set_trigger_effect(mounted && adaptive_triggers_enabled() ? kGunReady : TriggerEffect{});
}

nlohmann::json conker::status::adaptive_triggers() {
    return enhancement("adaptive_triggers", "Machine gun in the triggers and grips", adaptive_triggers_enabled(),
                       "CONKER_ADAPTIVE_TRIGGERS", {{"pad_has_adaptive_triggers", conker::input::has_adaptive_triggers()},
                        {"pad_has_haptics", conker::input::has_haptics()}});
}

conker::switches::Switch conker::switches::adaptive_triggers() {
    return {"ADAPTIVE TRIGGERS", adaptive_triggers_enabled, [](bool on) { g_enabled.store(on); }};
}
