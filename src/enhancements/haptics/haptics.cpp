// Haptics on the pad driven by the game's sound effects. Every sound effect starts in
// sndp_play_sound(player, sound, volume, flags), which the [[patches.hook]] in config/us.toml calls
// this from: chosen sounds play a haptic effect on the pad (src/platform/input.hpp: shaped on a
// DualSense on macOS, on the rumble motors of any other pad, Xbox trigger motors included). The
// volume is no guide to strength, since each sound has its own (Conker's reload click plays at
// 9233, his footsteps at 32767), so each effect has its own. The game's rumble pak still rumbles as before.
// CONKER_HAPTICS=0 turns them off.
//
// The sounds so far were picked out with the log below in scene 6 (Windy's slingshot B pad), by
// when they start. Landings are felt from Conker's animation instead, since their sound depends
// on what he lands on; conker_haptics_frame checks it once a frame (the hook in func_1501878C).
// The splat after a long fall and the squash under a box were found in scene 12 (debug reports
// scene12-20261023-155051, walking off the ledge, and scene12-20261023-155409, waiting).
//
// In a multiplayer match (D_800BE616 nonzero) each player's pad feels that player: player n is actor
// n (D_800CC2D0, 0x32C bytes each) and plays on controller n + 1, as for the mounted guns
// (src/enhancements/adaptive_triggers). Landings and splats are each player's own animation; a
// shotgun blast goes to the players holding a shotgun, the sound saying nothing of whose it is. The
// slingshot's sounds are single player's.
//
// CONKER_SOUND_LOG=1 prints each sound as it starts, with Conker's animation (the u16 at +0x84 of
// his actor, *D_80088720; in a match each player's), and each change of that animation, to find
// the ids of new ones.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <string>

#include "recomp.h"
#include "enhancements/status.hpp"
#include "enhancements/switches.hpp"
#include "platform/input.hpp"
#include "runtime/game.hpp"

namespace {

inline gpr kseg0(uint32_t address) { return static_cast<gpr>(static_cast<int32_t>(address)); }

constexpr uint32_t kConker = 0x80088720;      // D_80088720: Conker's actor
constexpr uint32_t kMultiplayer = 0x800BE616;  // D_800BE616: nonzero during a multiplayer match
constexpr uint32_t kActors = 0x800CC2D0;       // D_800CC2D0: the actors; player n is actor n in a match
constexpr uint32_t kActorSize = 0x32C;
constexpr uint32_t kActorAnimation = 0x84;    // u16
constexpr uint32_t kActorHealth = 0x1CA;      // u8
constexpr uint32_t kActorPlayer = 0x31C;      // the player's block

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
    // Footsteps (588-599 on stone) were tried and felt too strong for something so frequent.
};

// The shotgun (Spooky's mansion, with the red laser sight; out with B): the blast, felt as the
// mounted gun's rounds are, a hard hit in both grips and a short heavy rumble. Only while Conker
// holds the shotgun (his animations 0x221 to 0x237), in case the sounds are used elsewhere. Found
// in scene 60 with B then Z (debug reports scene60-20261024-221903 and -222712): fired as Z is
// pressed, sound 755 or 756 with animation 0x222, 0x236 or 0x237; zoomed in with R held, fired
// as Z is let go, with 0x227 or 0x237. The pump after it is 688.
bool shotgun_blast(uint16_t sound, uint16_t animation) {
    const bool blast = sound == 755 || sound == 756;
    return blast && animation >= 0x221 && animation <= 0x237;
}

constexpr Haptic kShotgun[] = {tap(1.0f, 0.5f, Grip::Both), buzz(1.0f, 0.1f, 0.15f, Grip::Both)};

// Conker landing on his feet from a jump, on any ground.
constexpr uint16_t kLandingAnimation = 0x36;
constexpr Haptic kLanding = tap(0.8f, 0.2f, Grip::Both);

// Conker hurt flat: splatting face down after a fall long enough to hurt him, after flailing on
// the way down (0xCF), or squashed flat by something landing on him (the box in scene 12; 0xF2 is
// him peeling himself up, 0xF6 dying if it was his last health). A hard hit, then a heavy rumble.
constexpr uint16_t kSplatAnimation = 0x27;
constexpr uint16_t kSquashAnimation = 0xF1;
constexpr Haptic kSplat[] = {tap(1.0f, 0.1f, Grip::Both), buzz(0.7f, 0.05f, 0.4f, Grip::Both)};

std::atomic<bool> g_enabled{[] {
    const char* value = std::getenv("CONKER_HAPTICS");
    return value == nullptr || value[0] != '0';
}()};  // the debug menu switches it

bool haptics_enabled() { return g_enabled.load(std::memory_order_relaxed); }

uint16_t g_last_animation[conker::input::kPorts] = {};  // game thread only, per player

const bool g_log = [] {
    const char* value = std::getenv("CONKER_SOUND_LOG");
    return value != nullptr && value[0] == '1';
}();

bool in_rdram(uint32_t address) { return address >= 0x80000000u && address < 0x80800000u; }

bool multiplayer(uint8_t* rdram) { return MEM_BU(0, kseg0(kMultiplayer)) != 0; }

// The actor whose events port `port` feels, or 0: Conker on controller 1 in the story, player n's
// on controller n + 1 in a match (an actor with a player's block).
uint32_t player_actor(uint8_t* rdram, int port) {
    if (!multiplayer(rdram)) {
        const uint32_t conker = static_cast<uint32_t>(MEM_W(0, kseg0(kConker)));
        return port == 0 && in_rdram(conker) ? conker : 0;
    }
    const uint32_t actor = kActors + static_cast<uint32_t>(port) * kActorSize;
    return in_rdram(static_cast<uint32_t>(MEM_W(kActorPlayer, kseg0(actor)))) ? actor : 0;
}

uint16_t animation_of(uint8_t* rdram, uint32_t actor) { return actor != 0 ? MEM_HU(kActorAnimation, kseg0(actor)) : 0; }

uint16_t conker_animation(uint8_t* rdram) { return animation_of(rdram, player_actor(rdram, 0)); }

void play_all(const Haptic* haptics, size_t count, int port) {
    for (size_t i = 0; i < count; i++) {
        conker::input::play_haptic(haptics[i], port);
    }
}

}  // namespace

// At sndp_play_sound's entry, on the game thread or the audio thread: $a1 is the sound (1 and
// up), $a2 its volume.
extern "C" void conker_sound_started(uint8_t* rdram, recomp_context* ctx) {
    const uint16_t sound = static_cast<uint16_t>(ctx->r5);
    const uint16_t volume = static_cast<uint16_t>(ctx->r6);
    const bool match = multiplayer(rdram);
    if (g_log) {
        std::printf("[conker] VI %llu: sound %u volume %u (%s animation 0x%X", static_cast<unsigned long long>(conker::current_vi()),
                    sound, volume, match ? "player animations" : "Conker", conker_animation(rdram));
        for (int port = 1; match && port < conker::input::kPorts; port++) {
            std::printf(" 0x%X", animation_of(rdram, player_actor(rdram, port)));
        }
        std::printf(")\n");
    }
    if (!haptics_enabled()) {
        return;
    }
    bool blast = false;
    for (int port = 0; port < conker::input::kPorts; port++) {
        const uint32_t actor = player_actor(rdram, port);
        if (actor != 0 && shotgun_blast(sound, animation_of(rdram, actor))) {
            blast = true;
            play_all(kShotgun, std::size(kShotgun), port);
        }
    }
    if (blast || match) {
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

// Once a frame, from func_1501878C before it runs the world frame (or skips it while paused).
extern "C" void conker_haptics_frame(uint8_t* rdram, recomp_context* /*ctx*/) {
    for (int port = 0; port < conker::input::kPorts; port++) {
        const uint32_t actor = player_actor(rdram, port);
        const uint16_t animation = animation_of(rdram, actor);
        if (animation == g_last_animation[port]) {
            continue;
        }
        if (g_log) {
            const std::string who = multiplayer(rdram) ? "player " + std::to_string(port + 1) : "Conker";
            std::printf("[conker] VI %llu: %s animation 0x%X (health %u)\n",
                        static_cast<unsigned long long>(conker::current_vi()), who.c_str(), animation,
                        actor != 0 ? MEM_BU(kActorHealth, kseg0(actor)) : 0u);
        }
        g_last_animation[port] = animation;
        if (!haptics_enabled()) {
            continue;
        }
        if (animation == kLandingAnimation) {
            conker::input::play_haptic(kLanding, port);
        } else if (animation == kSplatAnimation || animation == kSquashAnimation) {
            play_all(kSplat, std::size(kSplat), port);
        }
    }
}

nlohmann::json conker::status::haptics() {
    return enhancement("haptics", "Pad haptics from sound effects", haptics_enabled(), "CONKER_HAPTICS",
                       {{"pad_has_haptics", conker::input::has_haptics()}});
}

conker::switches::Switch conker::switches::haptics() {
    return {"HAPTICS", haptics_enabled, [](bool on) { g_enabled.store(on); }};
}
