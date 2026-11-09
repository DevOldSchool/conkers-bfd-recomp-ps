// Everything that shakes the pad beyond the game's own Rumble Pak: haptic effects for what happens to
// each player, and the feel of the weapon in their hand. The effects themselves are feel.cfg's
// (src/enhancements/feel); the triggers' resistance is src/enhancements/adaptive_triggers'.
// src/platform/input.hpp plays them: shaped on a DualSense on macOS, on the rumble motors of any other
// pad, Xbox trigger motors included. CONKER_HAPTICS=0 turns them off.
//
// Events:
// - Sounds: every sound effect starts in sndp_play_sound(player, sound, volume, flags), which the
//   [[patches.hook]] in config/us.toml calls conker_sound_started from. Chosen sounds play an effect:
//   the slingshot's, the shotgun's blast, and a weapon's own (<weapon>.sounds: the chainsaw's engine).
//   The volume is no guide to strength, since each sound has its own (Conker's reload click plays at
//   9233, his footsteps at 32767), so each effect has its own.
// - Animations, once a frame (conker_haptics_frame, the hook in func_1501878C): landing, a splat or a
//   squash, and a fall that kills. Landings are felt from Conker's animation rather than a sound,
//   since their sound depends on what he lands on.
// The sounds were picked out with the log below in scene 6 (Windy's slingshot B pad). The splat after
// a long fall and the squash under a box were found in scene 12 (debug reports
// scene12-20261023-155051, walking off the ledge, and scene12-20261023-155409, waiting).
//
// Weapons, once a frame, for the weapon in each player's hand:
// - the game's own rumble with the weapon's character (<weapon>.rumble), and a rumble of its own
//   while it is in hand (.hold) or used (.use: the chainsaw cutting);
// - a pulse on a beat (.pulse, .use_pulse: the Uzis pulsing from grip to grip as they fire), and a
//   shot each time the game's rumble starts (.shot);
// - seated at a mounted machine gun, a thump for every round fired (gun_round), from the rounds
//   counter at +0x1AA of the player's block (see src/enhancements/adaptive_triggers).
//
// In a multiplayer match each player's pad feels that player (src/enhancements/players.hpp).
// Landings and splats are each player's own animation; a shotgun blast goes to the players holding a
// shotgun, the sound saying nothing of whose it is. The slingshot's sounds are single player's.
//
// CONKER_SOUND_LOG=1 prints each sound as it starts, with Conker's animation (the u16 at +0x84 of
// his actor, *D_80088720; in a match each player's), and each change of that animation, to find
// the ids of new ones.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <string>

#include "net/net_play.hpp"
#include "enhancements/feel/feel.hpp"
#include "enhancements/feel/weapon_survey.hpp"
#include "enhancements/players.hpp"
#include "recomp.h"
#include "enhancements/status.hpp"
#include "enhancements/switches.hpp"
#include "platform/input.hpp"
#include "runtime/frame_input.hpp"
#include "runtime/game.hpp"

namespace {

namespace players = conker::players;
using players::kseg0;
using conker::input::Haptic;
using conker::input::RumbleFeel;
using Clock = std::chrono::steady_clock;

// The effect of the sounds first to last (variants of one sound are numbered together).
// Each effect itself is in conker-recomp-data/feel.cfg, by name (src/enhancements/feel): the
// built-in values are there too.
struct SoundHaptic {
    uint16_t first;
    uint16_t last;
    const char* feel;
};

constexpr SoundHaptic kSounds[] = {
    // Slingshot: taken out with B in the right hand, then fired with either trigger, in whose
    // grip the shot is felt.
    {1598, 1599, "slingshot_out"},      // taken out from a B pad, put away
    {1678, 1680, "slingshot_shot"},     // a shot (one of three, at random)
    {54, 54, "slingshot_elastic"},      // the elastic stretched again
    {17, 17, "slingshot_reload"},       // reloaded
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

constexpr const char* kShotgun = "shotgun";  // a hard hit in both grips and a short heavy rumble

// Conker landing on his feet from a jump, on any ground.
constexpr uint16_t kLandingAnimation = 0x36;
constexpr const char* kLanding = "landing";

// Conker hurt flat: splatting face down after a fall long enough to hurt him, after flailing on
// the way down (0xCF), or squashed flat by something landing on him (the box in scene 12; 0xF2 is
// him peeling himself up, 0xF6 dying if it was his last health). A hard hit, then a heavy rumble.
constexpr uint16_t kSplatAnimation = 0x27;
constexpr uint16_t kSquashAnimation = 0xF1;
constexpr const char* kSplat = "splat";  // a hard hit, then a heavy rumble

// Killed by a fall: flailing on the way down (0xCF), then 0x170 as the player hits the ground and their
// health goes to 0 (seen in a match with CONKER_SOUND_LOG=1: health 6 to 0 at once). Heavier and
// longer than a splat: the deepest hit there is, and a long rumble dying away.
constexpr uint16_t kFlailAnimation = 0xCF;
constexpr uint16_t kFallDeathAnimation = 0x170;
constexpr const char* kFallDeath = "fall_death";

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

// CONKER_FEEL_LOG=1: the item each player picks up, and the buttons pressed with it in hand.
const bool g_feel_log = [] {
    const char* value = std::getenv("CONKER_FEEL_LOG");
    return value != nullptr && value[0] == '1';
}();

uint16_t conker_animation(uint8_t* rdram) { return players::animation(rdram, players::actor(rdram, 0)); }

// Player `port`'s haptic on the controller they play with here: online that is this machine's
// controller 1 for its own player alone (a guest is player 2 or later), and nothing for the others.
void play_for(const Haptic& haptic, int port) {
    const int controller = conker::net::local_controller(port);
    if (controller >= 0 && conker::input::has_haptics(controller)) {
        conker::input::play_haptic(haptic, controller);
    }
}

// An event's effects from feel.cfg, every part at once.
void play_all(const char* feel, int port) {
    for (const Haptic& haptic : conker::feel::haptic(feel)) {
        play_for(haptic, port);
    }
}

// --- weapons ---

// Each player's weapon state. Game thread only.
struct Weapon {
    std::string name;                // in hand last frame, logged as it changes
    bool mounted = false;            // seated at a mounted gun last frame
    int16_t rounds = 0;              // its rounds counter then
    bool round_right = false;        // the next round's grip, alternating
    Clock::time_point next_pulse;    // the pulse's next beat
    bool pulse_right = false;
    bool was_rumbling = false;       // the game's rumble last frame, for .shot
    bool shot_right = false;
    uint16_t buttons = 0;            // pressed last frame, logged as they change
    Clock::time_point last_use;      // its use button last held
    Clock::time_point last_rumble;   // the game's rumble last on
};
Weapon g_weapons[conker::input::kPorts];

// Seated at a mounted gun: a thump for each round fired since the last frame, in both grips, or with
// gun_round.alternate in one grip a round, left then right, so the gun kicks from side to side.
void gun_rounds(uint8_t* rdram, uint32_t block, int port) {
    Weapon& w = g_weapons[port];
    const int16_t rounds = static_cast<int16_t>(MEM_HU(players::kPlayerGunRounds, kseg0(block)));
    if (w.mounted && rounds != w.rounds) {
        const bool alternate = conker::feel::number("gun_round.alternate") != 0.0f;
        const Haptic::Grip side = w.round_right ? Haptic::Grip::Right : Haptic::Grip::Left;
        w.round_right = !w.round_right;
        for (Haptic haptic : conker::feel::haptic("gun_round")) {
            if (alternate && haptic.grip == Haptic::Grip::Both) haptic.grip = side;
            play_for(haptic, port);
        }
    }
    w.rounds = rounds;
    w.mounted = true;
}

// A weapon's pulse (.pulse, or .use_pulse while used), on its beat, alternating grips if asked.
void play_pulse(const std::string& weapon, bool in_use, int port) {
    Weapon& w = g_weapons[port];
    conker::feel::Pulse pulse;
    const auto now = Clock::now();
    if (!conker::feel::pulse(weapon, in_use, &pulse)) {
        w.next_pulse = now;
        return;
    }
    if (now < w.next_pulse) return;
    // Behind by more than a beat (just picked up, or the game paused): start from now.
    const auto beat = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float>(pulse.every));
    w.next_pulse = (now - w.next_pulse > beat ? now : w.next_pulse) + beat;
    Haptic haptic = pulse.haptic;
    if (pulse.alternate) {
        haptic.grip = w.pulse_right ? Haptic::Grip::Right : Haptic::Grip::Left;
        w.pulse_right = !w.pulse_right;
    }
    play_for(haptic, port);
}

// A weapon's shot (.shot) each time the game's own rumble starts with it in hand: the game rumbles
// once a shot for most guns.
void play_shot(const std::string& weapon, bool rumbling, int port) {
    Weapon& w = g_weapons[port];
    const bool started = rumbling && !w.was_rumbling;
    w.was_rumbling = rumbling;
    conker::feel::Pulse shot;
    if (!started || !conker::feel::shot(weapon, &shot)) return;
    Haptic haptic = shot.haptic;
    if (shot.alternate) {
        haptic.grip = w.shot_right ? Haptic::Grip::Right : Haptic::Grip::Left;
        w.shot_right = !w.shot_right;
    }
    play_for(haptic, port);
}

// The weapon in a player's hand: the game's rumble with its character, its own hold or use rumble,
// its pulse and its shots.
void weapon_in_hand(const std::string& weapon, int port, RumbleFeel* rumble, bool* holding, RumbleFeel* hold) {
    Weapon& w = g_weapons[port];
    *rumble = conker::feel::rumble(weapon.c_str());
    *holding = conker::feel::hold(weapon, hold);
    // Used (the chainsaw cutting): its use rumble while its button is held, as the game sees the pad.
    RumbleFeel used;
    char button = 'z';
    const bool has_use = conker::feel::use(weapon, &used, &button);
    const uint16_t mask = conker::feel::use_buttons(weapon);
    const uint16_t buttons = conker::frame_input::port(port).buttons;
    // Which buttons are pressed with a weapon in hand, as they change: for finding its use button.
    const uint16_t pressed = buttons & (conker::input::kA | conker::input::kB | conker::input::kZ | conker::input::kR);
    if (pressed != w.buttons) {
        w.buttons = pressed;
        if (g_feel_log) std::printf("[conker] feel: player %d, %s in hand, presses%s%s%s%s%s (use: %s)\n", port + 1, weapon.c_str(),
                    pressed & conker::input::kA ? " A" : "", pressed & conker::input::kB ? " B" : "",
                    pressed & conker::input::kZ ? " Z" : "", pressed & conker::input::kR ? " R" : "",
                    pressed == 0 ? " nothing" : "", (pressed & mask) != 0 ? "yes" : "no");
    }
    // Used while the button is held, and for the weapon's .use_linger seconds after (a cut runs on).
    const auto now = Clock::now();
    if ((buttons & mask) != 0) w.last_use = now;
    const float linger = conker::feel::number((weapon + ".use_linger").c_str());
    bool in_use = w.last_use != Clock::time_point{} && now - w.last_use <= std::chrono::duration<float>(linger);
    // A gun only fires while the game rumbles for its shots (not while it reloads, the trigger still
    // held): its .use_rumble_window seconds since the game's rumble was last on, 0 for no such test.
    const int controller = conker::net::local_controller(port);
    const bool rumbling = controller >= 0 && conker::input::rumble_on(controller);
    if (rumbling) w.last_rumble = now;
    const float window = conker::feel::number((weapon + ".use_rumble_window").c_str());
    if (window > 0.0f && now - w.last_rumble > std::chrono::duration<float>(window)) {
        in_use = false;
    }
    if (has_use && in_use) {
        *holding = true;
        *hold = used;
    }
    play_pulse(weapon, in_use, port);
    play_shot(weapon, rumbling, port);
}

// Once a frame, each player's weapon: what it does to the pad while in hand, or a mounted gun's rounds.
void weapons(uint8_t* rdram) {
    for (int port = 0; port < conker::input::kPorts; port++) {
        Weapon& w = g_weapons[port];
        const uint32_t actor = players::armed_actor(rdram, port);
        const uint32_t block = players::block(rdram, actor);
        const bool mounted = players::mounted(rdram, block);
        const std::string weapon = mounted ? std::string{} : players::held_weapon(rdram, block);
        if (weapon != w.name) {
            w.name = weapon;
            if (!weapon.empty() && g_feel_log) {
                std::printf("[conker] feel: player %d holds item 0x%02X (%s in feel.cfg)\n", port + 1,
                            MEM_BU(players::kPlayerItem, kseg0(block)), weapon.c_str());
            }
        }
        RumbleFeel rumble;  // the deep default
        RumbleFeel hold;
        bool holding = false;
        if (!mounted) w.mounted = false;
        if (haptics_enabled()) {
            if (mounted) {
                gun_rounds(rdram, block, port);
            } else if (!weapon.empty()) {
                weapon_in_hand(weapon, port, &rumble, &holding, &hold);
            }
        }
        const int controller = conker::net::local_controller(port);
        if (controller >= 0) {
            conker::input::set_rumble_feel(controller, rumble);
            conker::input::set_hold_rumble(controller, holding, hold);
        }
    }
}

}  // namespace

// At sndp_play_sound's entry, on the game thread or the audio thread: $a1 is the sound (1 and
// up), $a2 its volume.
extern "C" void conker_sound_started(uint8_t* rdram, recomp_context* ctx) {
    const uint16_t sound = static_cast<uint16_t>(ctx->r5);
    const uint16_t volume = static_cast<uint16_t>(ctx->r6);
    conker::survey::sound(rdram, sound);
    const bool match = players::multiplayer(rdram);
    if (g_log) {
        std::printf("[conker] VI %llu: sound %u volume %u (%s animation 0x%X", static_cast<unsigned long long>(conker::current_vi()),
                    sound, volume, match ? "player animations" : "Conker", conker_animation(rdram));
        for (int port = 1; match && port < conker::input::kPorts; port++) {
            std::printf(" 0x%X", players::animation(rdram, players::actor(rdram, port)));
        }
        std::printf(")\n");
    }
    if (!haptics_enabled()) {
        return;
    }
    bool blast = false;
    for (int port = 0; port < conker::input::kPorts; port++) {
        const uint32_t actor = players::actor(rdram, port);
        // In a match every player's shots are heard by all, and other guns' animations can share the
        // shotgun's range: only a player holding the shotgun feels its blast.
        if (match && players::held_weapon(rdram, players::block(rdram, players::armed_actor(rdram, port))) != kShotgun) {
            continue;
        }
        if (actor != 0 && shotgun_blast(sound, players::animation(rdram, actor))) {
            blast = true;
            play_all(kShotgun, port);
        }
    }
    // A weapon whose sounds rumble (feel.cfg's <weapon>.sounds: the chainsaw's engine), for each player
    // holding it. In a match a sound is heard by all, so another player's chainsaw can be felt too.
    for (int port = 0; port < conker::input::kPorts; port++) {
        const std::string weapon = players::held_weapon(rdram, players::block(rdram, players::actor(rdram, port)));
        if (weapon.empty()) continue;
        for (const Haptic& haptic : conker::feel::sound_haptic(weapon, sound)) {
            play_for(haptic, port);
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
    for (const Haptic& haptic : conker::feel::haptic(it->feel)) {
        conker::input::play_haptic(haptic);
    }
}

// Once a frame, from func_1501878C before it runs the world frame (or skips it while paused).
extern "C" void conker_haptics_frame(uint8_t* rdram, recomp_context* /*ctx*/) {
    // With CONKER_FEEL_LIVE=1, feel.cfg saved while playing is taken in now, even with nothing playing.
    conker::feel::refresh();
    conker::survey::frame(rdram);
    weapons(rdram);
    for (int port = 0; port < conker::input::kPorts; port++) {
        const uint32_t actor = players::actor(rdram, port);
        const uint16_t animation = players::animation(rdram, actor);
        if (animation == g_last_animation[port]) {
            continue;
        }
        if (g_log) {
            const std::string who = players::multiplayer(rdram) ? "player " + std::to_string(port + 1) : "Conker";
            std::printf("[conker] VI %llu: %s animation 0x%X (health %u)\n",
                        static_cast<unsigned long long>(conker::current_vi()), who.c_str(), animation,
                        actor != 0 ? MEM_BU(players::kActorHealth, kseg0(actor)) : 0u);
        }
        const uint16_t previous = g_last_animation[port];
        g_last_animation[port] = animation;
        if (!haptics_enabled()) {
            continue;
        }
        if (animation == kLandingAnimation) {
            play_all(kLanding, port);
        } else if (animation == kSplatAnimation || animation == kSquashAnimation) {
            play_all(kSplat, port);
        } else if (animation == kFallDeathAnimation && previous == kFlailAnimation) {
            play_all(kFallDeath, port);
        }
    }
}

nlohmann::json conker::status::haptics() {
    return enhancement("haptics", "Pad haptics for events and the weapon in hand", haptics_enabled(), "CONKER_HAPTICS",
                       {{"pad_has_haptics", conker::input::has_haptics()}});
}

conker::switches::Switch conker::switches::haptics() {
    // Each player's own online: it only shakes this machine's pad, from game memory it never writes.
    return {"HAPTICS", haptics_enabled, [](bool on) { g_enabled.store(on); }, true};
}
