// A DualSense's triggers (both are Z): how they resist for what each player is doing. The effects
// themselves are feel.cfg's (src/enhancements/feel); what shakes the pad is
// src/enhancements/haptics/haptics.cpp's.
//
// - Seated at a mounted machine gun: the triggers pull heavy (mounted_gun.trigger).
// - A weapon in hand: its own <weapon>.trigger (the Uzis' light resistance, say).
// - Peeing while drunk (B on the B pad gets him drunk, B starts and stops the pee, Z sends it
//   further): the triggers resist in step with the stream's power, light for the plain stream and
//   firm when Z has pushed it as far as it goes, easing again as it falls back. Story only.
//
// In a multiplayer match every player's triggers follow that player (src/enhancements/players.hpp).
//
// Native helper called once a frame from the [[patches.hook]] in config/us.toml (func_1501878C);
// src/platform/input.hpp sends the effect. CONKER_ADAPTIVE_TRIGGERS=0 turns it off.
//
// The mounted gun's state is in the player's block at +0x31C of Conker's actor (*D_80088720), the
// one func_150BDB70 reads to aim a mounted gun at where the player looks. Found in the wasps' hive
// (scene 39, debug report scene39-20261024-144343) by dumping RAM before, during and after
// holding Z:
// - u8 +0x197 is 2 while he is mounted on the gun (0 on foot, with the slingshot too);
// - u8 +0x199 is 1 while Z is held on it;
// - u16 +0x19E counts the spin-up down from 150, a step a frame, and the gun fires at 0. It stays
//   0 for a while after Z is let go, so a quick second press fires at once;
// - s16 +0x1AA counts the rounds fired, about one every 10-14 VIs; the barrels go on firing for
//   about half a second after Z is let go.
//
// The pee stream is a heap object made when he starts peeing, owned by his actor; nothing outside
// the heap points to it, so it is found by walking the heap's blocks (12-byte headers {next, prev,
// tag << 24 | size}, from D_800380B4), as Rare's own func_80004250 does every frame. Found in
// Rock Solid (scene 49, debug report scene49-20261024-114625: walk right onto the B pad, B, wait
// for the drunk cutscene, B) by dumping RAM before and while holding Z:
// - its first word's upper half is 0x2201 (the lower half varies, as does the block's size, about
//   0x800 bytes) and +0xA0 is its owner, Conker's actor;
// - f32 +0xDC is the stream's power: 0.0172 for the plain stream, climbing about 0.0007 a VI
//   while Z is held to 0.0429 (about 45 VIs), and falling back at the same rate when Z is let go.
//   The stream lands about 460 units further at full power.
// Conker's animation (u16 +0x84) is 0x2E starting to pee, 0x2C peeing and 0x2D stopping.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include "recomp.h"
#include "enhancements/feel/feel.hpp"
#include "enhancements/players.hpp"
#include "enhancements/status.hpp"
#include "enhancements/switches.hpp"
#include "net/net_play.hpp"
#include "platform/input.hpp"

namespace {

namespace players = conker::players;
using players::kseg0;
using conker::input::TriggerEffect;

constexpr uint32_t kHeapFirst = 0x800380B4;    // D_800380B4: the heap's first block
constexpr uint32_t kHeapLast = 0x80038098;     // D_80038098: its last
constexpr uint32_t kHeapHeader = 0xC;
constexpr int kHeapMaxBlocks = 4096;           // a few hundred in play; stops a damaged list
constexpr uint32_t kStreamKind = 0x2201;       // its first word's upper half
constexpr uint32_t kStreamOwner = 0xA0;
constexpr uint32_t kStreamPower = 0xDC;        // f32
constexpr float kStreamPlain = 0.0172f;
constexpr float kStreamFull = 0.0429f;

constexpr uint16_t kPeeStart = 0x2E;
constexpr uint16_t kPeeing = 0x2C;

constexpr TriggerEffect feedback(float from, float strength) {
    TriggerEffect effect;
    effect.mode = TriggerEffect::Mode::Feedback;
    for (int i = 0; i < conker::input::kTriggerZones; i++) {
        effect.zones[i] = static_cast<float>(i) >= from * conker::input::kTriggerZones ? strength : 0.0f;
    }
    return effect;
}

// Peeing: free for the first part of the pull, then a resistance that grows with the stream's power
// (feel.cfg's pee.from, pee.min and pee.max), in a few steps rather than a new effect every frame.
constexpr float kPeeSteps = 8.0f;
TriggerEffect pee_effect(float power) {
    power = std::floor(std::clamp(power, 0.0f, 1.0f) * kPeeSteps) / kPeeSteps;
    const float low = conker::feel::number("pee.min");
    const float high = conker::feel::number("pee.max");
    return feedback(conker::feel::number("pee.from"), low + (high - low) * power);
}

std::atomic<bool> g_enabled{[] {
    const char* value = std::getenv("CONKER_ADAPTIVE_TRIGGERS");
    return value == nullptr || value[0] != '0';
}()};  // the debug menu switches it

bool adaptive_triggers_enabled() { return g_enabled.load(std::memory_order_relaxed); }

// The pee stream's power from 0 (the plain stream) to 1 (as far as Z sends it), or -1 when Conker
// has no stream.
float pee_power(uint8_t* rdram, uint32_t conker) {
    uint32_t block = static_cast<uint32_t>(MEM_W(0, kseg0(kHeapFirst)));
    const uint32_t last = static_cast<uint32_t>(MEM_W(0, kseg0(kHeapLast)));
    for (int i = 0; i < kHeapMaxBlocks && players::in_rdram(block); i++) {
        const uint32_t data = block + kHeapHeader;
        if (static_cast<uint32_t>(MEM_W(0, kseg0(data))) >> 16 == kStreamKind &&
            static_cast<uint32_t>(MEM_W(kStreamOwner, kseg0(data))) == conker) {
            const uint32_t bits = static_cast<uint32_t>(MEM_W(kStreamPower, kseg0(data)));
            float power;
            std::memcpy(&power, &bits, sizeof(power));
            if (power > kStreamPlain * 0.5f && power < kStreamFull * 1.5f) {
                return (power - kStreamPlain) / (kStreamFull - kStreamPlain);
            }
        }
        if (block == last) {
            break;
        }
        block = static_cast<uint32_t>(MEM_W(0, kseg0(block)));
    }
    return -1.0f;
}

// The trigger effect for player `port` (off when nothing calls for one).
TriggerEffect effect_for(uint8_t* rdram, int port) {
    const uint32_t actor = players::armed_actor(rdram, port);
    const uint32_t block = players::block(rdram, actor);
    if (block == 0) return {};
    if (players::mounted(rdram, block)) return conker::feel::trigger("mounted_gun");
    TriggerEffect effect;
    if (const std::string weapon = players::held_weapon(rdram, block); !weapon.empty()) {
        effect = conker::feel::trigger(weapon.c_str());
    }
    if (!players::multiplayer(rdram)) {
        const uint16_t animation = players::animation(rdram, actor);
        if (animation == kPeeStart || animation == kPeeing) {
            if (const float power = pee_power(rdram, actor); power >= 0.0f) {
                effect = pee_effect(power);
            }
        }
    }
    return effect;
}

} // namespace

// Once a frame, from func_1501878C just before it decides whether to run the world frame.
extern "C" void conker_adaptive_triggers(uint8_t* rdram, recomp_context* /*ctx*/) {
    const bool match = players::multiplayer(rdram);
    for (int port = 0; port < conker::input::kPorts; port++) {
        const TriggerEffect effect = adaptive_triggers_enabled() ? effect_for(rdram, port) : TriggerEffect{};
        // Online, player n's controller is this machine's controller 1 on their machine alone (a guest
        // is player 2 or later, and its pad is still controller 1). In the story the other pads are
        // freed, as a match may have left them.
        const int controller = match ? conker::net::local_controller(port) : port;
        if (controller >= 0) {
            conker::input::set_trigger_effect(effect, controller);
        }
    }
}

nlohmann::json conker::status::adaptive_triggers() {
    return enhancement("adaptive_triggers", "Triggers that resist for guns, weapons and peeing", adaptive_triggers_enabled(),
                       "CONKER_ADAPTIVE_TRIGGERS", {{"pad_has_adaptive_triggers", conker::input::has_adaptive_triggers()}});
}

conker::switches::Switch conker::switches::adaptive_triggers() {
    // Each player's own online: it only sets this machine's pad, from game memory it never writes.
    return {"ADAPTIVE TRIGGERS", adaptive_triggers_enabled, [](bool on) { g_enabled.store(on); }, true};
}
