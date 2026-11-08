// A DualSense's triggers (both are Z) for the two things Conker does by holding Z: peeing harder
// while he's drunk, and firing the mounted machine gun.
//
// Peeing (B on the B pad gets him drunk, B starts and stops the pee, Z sends it further): the
// triggers resist in step with the stream's power, light for the plain stream and firm when Z has
// pushed it as far as it goes, easing again as it falls back.
//
// The mounted machine gun, while Conker sits at it: the triggers pull heavy, and every round it
// fires thumps in both grips, as the gun is held with both hands. The triggers only resist; the
// shaking is in the grips, and only for the rounds: nothing while the barrels wind up, and a gap
// between thumps so each round is felt on its own.
//
// In a multiplayer match (D_800BE616 nonzero) every player's gun is followed, whoever they play:
// player n is actor n (D_800CC2D0, 0x32C bytes each, as the match's own tallies walk them) and plays
// on controller n + 1, whose triggers and grips are that player's own.
//
// Native helper called once a frame from the [[patches.hook]] in config/us.toml (func_1501878C);
// src/platform/input.hpp sends the trigger effect and plays the thumps (Core Haptics on a DualSense
// on macOS, the motors elsewhere). CONKER_ADAPTIVE_TRIGGERS=0 turns it all off.
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
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "recomp.h"
#include "enhancements/feel/feel.hpp"
#include "runtime/frame_input.hpp"

#include "platform/input.hpp"
#include "runtime/game.hpp"
#include "net/net_play.hpp"
#include "enhancements/status.hpp"
#include "enhancements/switches.hpp"

namespace {

inline gpr kseg0(uint32_t address) { return static_cast<gpr>(static_cast<int32_t>(address)); }

constexpr uint32_t kConker = 0x80088720;       // D_80088720: Conker's actor
constexpr uint32_t kWorldPaused = 0x800BEAC0;  // D_800BEAC0: the world frame is skipped (pause, menus)
constexpr uint32_t kMultiplayer = 0x800BE616;  // D_800BE616: nonzero during a multiplayer match
constexpr uint32_t kActors = 0x800CC2D0;       // D_800CC2D0: the actors; player n is actor n in a match
constexpr uint32_t kActorSize = 0x32C;
constexpr uint32_t kActorModel = 0x4;          // u8: 0 for Conker
constexpr uint32_t kActorAnimation = 0x84;     // u16
constexpr uint32_t kActorPlayer = 0x31C;       // the player's block

constexpr uint32_t kPlayerItem = 0x78;         // u8: the item held
constexpr uint32_t kPlayerItemState = 0x11A;   // u8: kItemHeld while it is in hand
constexpr uint32_t kPlayerMounted = 0x197;     // u8
constexpr uint32_t kPlayerGunRounds = 0x1AA;   // s16

constexpr uint8_t kMountedGun = 2;
constexpr uint8_t kItemHeld = 2;
// The weapons by the item a player holds (player block +0x78, in hand while +0x11A is kItemHeld), each
// with its feel.cfg lines <name>.trigger and <name>.rumble, applied while it is in hand. An item not
// named here is item_<hex> (item_2a...), so any of them can be given lines. Names as consistent
// controls and the debug reports found them; the item_ ones are guns the game lists but nobody has
// named yet (the log names the item in hand as it changes).
// (the weapons by item are feel::weapon_name's table)

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

using conker::input::TriggerEffect;

constexpr TriggerEffect feedback(float from, float strength) {
    TriggerEffect effect;
    effect.mode = TriggerEffect::Mode::Feedback;
    for (int i = 0; i < conker::input::kTriggerZones; i++) {
        effect.zones[i] = static_cast<float>(i) >= from * conker::input::kTriggerZones ? strength : 0.0f;
    }
    return effect;
}

// Seated at the gun: free for the first fifth of the pull, then heavy all the way down (feel.cfg's
// mounted_gun.trigger).

// Holding the Uzis: a little resistance past the first fifth, lighter than the mounted gun's, and the
// game's rumble as they fire sharper and lighter than its usual deep rumble.
// (feel.cfg's uzis.trigger and uzis.rumble)

// Peeing: free for the first fifth of the pull, then a resistance that grows with the stream's
// power, in a few steps rather than a new effect every frame.
constexpr float kPeeSteps = 8.0f;
TriggerEffect pee_effect(float power) {
    power = std::floor(std::clamp(power, 0.0f, 1.0f) * kPeeSteps) / kPeeSteps;
    const float low = conker::feel::number("pee.min");
    const float high = conker::feel::number("pee.max");
    return feedback(conker::feel::number("pee.from"), low + (high - low) * power);
}

using conker::input::Haptic;

// A round going off, in both grips: a sharp hit on top of a short, deep thump, which together feel
// heavier than either at full strength alone. The thump's sharpness 0.15 is deep on a DualSense and
// still runs the light motor of a pad that has two (at 0 only the heavy one would turn). Rounds come
// 8-14 VIs (130-230 ms) apart, so an 80 ms thump leaves a gap before the next.
// (feel.cfg's gun_round)

std::atomic<bool> g_enabled{[] {
    const char* value = std::getenv("CONKER_ADAPTIVE_TRIGGERS");
    return value == nullptr || value[0] != '0';
}()};  // the debug menu switches it

bool adaptive_triggers_enabled() { return g_enabled.load(std::memory_order_relaxed); }

bool in_rdram(uint32_t address) { return address >= 0x80000000u && address < 0x80800000u; }

// Conker's actor while he is in the running world, or 0.
uint32_t conker_actor(uint8_t* rdram) {
    if (MEM_BU(0, kseg0(kWorldPaused)) != 0) {
        return 0;
    }
    const uint32_t conker = static_cast<uint32_t>(MEM_W(0, kseg0(kConker)));
    return in_rdram(conker) && MEM_BU(kActorModel, kseg0(conker)) == 0 ? conker : 0;
}

// The player's block, or 0.
uint32_t player_block(uint8_t* rdram, uint32_t conker) {
    const uint32_t player = static_cast<uint32_t>(MEM_W(kActorPlayer, kseg0(conker)));
    return in_rdram(player) ? player : 0;
}

// The pee stream's power from 0 (the plain stream) to 1 (as far as Z sends it), or -1 when Conker
// has no stream.
float pee_power(uint8_t* rdram, uint32_t conker) {
    uint32_t block = static_cast<uint32_t>(MEM_W(0, kseg0(kHeapFirst)));
    const uint32_t last = static_cast<uint32_t>(MEM_W(0, kseg0(kHeapLast)));
    for (int i = 0; i < kHeapMaxBlocks && in_rdram(block); i++) {
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

// Game thread only, per controller.
int16_t g_last_rounds[conker::input::kPorts] = {};
bool g_was_mounted[conker::input::kPorts] = {};
bool g_round_right[conker::input::kPorts] = {};

// A thump for each round fired since the last frame: in both grips of controller `port`, or with
// gun_round.alternate in one grip a round, left then right, so the gun kicks from side to side.
void gun_rumble(uint8_t* rdram, uint32_t player, int port) {
    const int16_t rounds = static_cast<int16_t>(MEM_HU(kPlayerGunRounds, kseg0(player)));
    const int controller = conker::net::local_controller(port);  // online, this machine's port only
    if (g_was_mounted[port] && rounds != g_last_rounds[port] && controller >= 0 && conker::input::has_haptics(controller)) {
        const bool alternate = conker::feel::number("gun_round.alternate") != 0.0f;
        const Haptic::Grip side = g_round_right[port] ? Haptic::Grip::Right : Haptic::Grip::Left;
        g_round_right[port] = !g_round_right[port];
        for (Haptic haptic : conker::feel::haptic("gun_round")) {
            if (alternate && haptic.grip == Haptic::Grip::Both) haptic.grip = side;
            conker::input::play_haptic(haptic, controller);
        }
    }
    g_last_rounds[port] = rounds;
    g_was_mounted[port] = true;
}

// The feel.cfg name of the weapon a player holds, or "" with nothing in hand.
std::string held_weapon(uint8_t* rdram, uint32_t player) {
    if (player == 0 || MEM_BU(kPlayerItemState, kseg0(player)) != kItemHeld) {
        return {};
    }
    return conker::feel::weapon_name(MEM_BU(kPlayerItem, kseg0(player)));
}

// The weapon in each player's hand, last frame: logged as it changes, for naming feel.cfg's lines.
std::string g_last_weapon[conker::input::kPorts];

// A weapon's pulse (feel.cfg's .pulse / .use_pulse), on its beat, alternating grips if asked. Game
// thread only, per player.
std::chrono::steady_clock::time_point g_next_pulse[conker::input::kPorts];
bool g_pulse_right[conker::input::kPorts] = {};
void play_pulse(const std::string& weapon, bool in_use, int port) {
    conker::feel::Pulse pulse;
    const auto now = std::chrono::steady_clock::now();
    if (!conker::feel::pulse(weapon, in_use, &pulse)) {
        g_next_pulse[port] = now;
        return;
    }
    if (now < g_next_pulse[port]) return;
    // Behind by more than a beat (just picked up, or the game paused): start from now.
    const auto beat = std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<float>(pulse.every));
    g_next_pulse[port] = (now - g_next_pulse[port] > beat ? now : g_next_pulse[port]) + beat;
    conker::input::Haptic haptic = pulse.haptic;
    if (pulse.alternate) {
        haptic.grip = g_pulse_right[port] ? conker::input::Haptic::Grip::Right : conker::input::Haptic::Grip::Left;
        g_pulse_right[port] = !g_pulse_right[port];
    }
    const int controller = conker::net::local_controller(port);
    if (controller >= 0 && conker::input::has_haptics(controller)) {
        conker::input::play_haptic(haptic, controller);
    }
}

// A weapon's shot haptic (feel.cfg's .shot) each time the game's own rumble starts with it in hand:
// the game rumbles once a shot for most guns (the Uzis about once a round). Game thread only.
bool g_was_rumbling[conker::input::kPorts] = {};
bool g_shot_right[conker::input::kPorts] = {};
void play_shot(const std::string& weapon, int port) {
    const int controller = conker::net::local_controller(port);
    if (controller < 0) return;
    const bool rumbling = conker::input::rumble_on(controller);
    const bool started = rumbling && !g_was_rumbling[port];
    g_was_rumbling[port] = rumbling;
    conker::feel::Pulse shot;
    if (!started || !conker::feel::shot(weapon, &shot) || !conker::input::has_haptics(controller)) return;
    conker::input::Haptic haptic = shot.haptic;
    if (shot.alternate) {
        haptic.grip = g_shot_right[port] ? conker::input::Haptic::Grip::Right : conker::input::Haptic::Grip::Left;
        g_shot_right[port] = !g_shot_right[port];
    }
    conker::input::play_haptic(haptic, controller);
}

// The trigger and rumble feel of the weapon a player holds (none: free triggers, the deep rumble).
void weapon_feel(uint8_t* rdram, uint32_t player, int port, TriggerEffect* effect, conker::input::RumbleFeel* rumble,
                 bool* holding, conker::input::RumbleFeel* hold) {
    const std::string weapon = held_weapon(rdram, player);
    if (weapon != g_last_weapon[port]) {
        g_last_weapon[port] = weapon;
        if (!weapon.empty()) {
            std::printf("[conker] feel: player %d holds item 0x%02X (%s in feel.cfg)\n", port + 1,
                        MEM_BU(kPlayerItem, kseg0(player)), weapon.c_str());
        }
    }
    if (weapon.empty() || !adaptive_triggers_enabled()) {
        return;
    }
    *effect = conker::feel::trigger(weapon.c_str());
    *rumble = conker::feel::rumble(weapon.c_str());
    *holding = conker::feel::hold(weapon, hold);
    // Used (the chainsaw cutting): its use rumble while its button is held, as the game sees the pad.
    conker::input::RumbleFeel used;
    char button = 'z';
    const bool has_use = conker::feel::use(weapon, &used, &button);
    const uint16_t mask = conker::feel::use_buttons(weapon);
    // Which buttons are pressed with a weapon in hand, as they change: for finding its use button.
    static uint16_t last_buttons[conker::input::kPorts] = {};
    const uint16_t pressed = conker::frame_input::port(port).buttons &
                             (conker::input::kA | conker::input::kB | conker::input::kZ | conker::input::kR);
    if (pressed != last_buttons[port]) {
        last_buttons[port] = pressed;
        std::printf("[conker] feel: player %d, %s in hand, presses%s%s%s%s%s (use: %s)\n", port + 1, weapon.c_str(),
                    pressed & conker::input::kA ? " A" : "", pressed & conker::input::kB ? " B" : "",
                    pressed & conker::input::kZ ? " Z" : "", pressed & conker::input::kR ? " R" : "",
                    pressed == 0 ? " nothing" : "", (pressed & mask) != 0 ? "yes" : "no");
    }
    // Used while the button is held, and for the weapon's .use_linger seconds after (a cut runs on).
    static std::chrono::steady_clock::time_point last_use[conker::input::kPorts];
    const auto now = std::chrono::steady_clock::now();
    if ((conker::frame_input::port(port).buttons & mask) != 0) last_use[port] = now;
    const float linger = conker::feel::number((weapon + ".use_linger").c_str());
    bool in_use = now - last_use[port] <= std::chrono::duration<float>(linger) &&
                  last_use[port] != std::chrono::steady_clock::time_point{};
    // A gun only fires while the game rumbles for its shots (not while it reloads, the trigger still
    // held): its .use_rumble_window seconds since the game's rumble was last on, 0 for no such test.
    static std::chrono::steady_clock::time_point last_rumble[conker::input::kPorts];
    const int controller = conker::net::local_controller(port);
    if (controller >= 0 && conker::input::rumble_on(controller)) last_rumble[port] = now;
    const float window = conker::feel::number((weapon + ".use_rumble_window").c_str());
    if (window > 0.0f && now - last_rumble[port] > std::chrono::duration<float>(window)) {
        in_use = false;
    }
    if (has_use && in_use) {
        *holding = true;
        *hold = used;
    }
    play_pulse(weapon, in_use, port);
    play_shot(weapon, port);
}

// A multiplayer match: each player's gun on their own controller.
void multiplayer_guns(uint8_t* rdram) {
    const bool running = MEM_BU(0, kseg0(kWorldPaused)) == 0;
    for (int i = 0; i < conker::input::kPorts; i++) {
        TriggerEffect effect;  // off
        const uint32_t player = running ? player_block(rdram, kActors + i * kActorSize) : 0;
        const bool mounted = player != 0 && MEM_BU(kPlayerMounted, kseg0(player)) == kMountedGun;
        if (mounted && adaptive_triggers_enabled()) {
            gun_rumble(rdram, player, i);
            effect = conker::feel::trigger("mounted_gun");
        }
        conker::input::RumbleFeel rumble;
        conker::input::RumbleFeel hold;
        bool holding = false;
        if (!mounted) {
            g_was_mounted[i] = false;
            weapon_feel(rdram, player, i, &effect, &rumble, &holding, &hold);
        }
        // Online, player n's controller is this machine's controller 1 on their machine alone (a guest
        // is player 2 or later, and its pad is still controller 1): the effect went to a controller 2
        // that is not there.
        const int controller = conker::net::local_controller(i);
        if (controller >= 0) {
            conker::input::set_trigger_effect(effect, controller);
            conker::input::set_rumble_feel(controller, rumble);
            conker::input::set_hold_rumble(controller, holding, hold);
        }
    }
}

} // namespace

// Once a frame, from func_1501878C just before it decides whether to run the world frame.
extern "C" void conker_adaptive_triggers(uint8_t* rdram, recomp_context* /*ctx*/) {
    if (MEM_BU(0, kseg0(kMultiplayer)) != 0) {
        multiplayer_guns(rdram);
        return;
    }
    TriggerEffect effect;  // off
    const uint32_t conker = conker_actor(rdram);
    const uint32_t player = conker != 0 ? player_block(rdram, conker) : 0;
    const bool mounted = player != 0 && MEM_BU(kPlayerMounted, kseg0(player)) == kMountedGun;
    if (mounted && adaptive_triggers_enabled()) {
        gun_rumble(rdram, player, 0);
        effect = conker::feel::trigger("mounted_gun");
    }
    conker::input::RumbleFeel rumble;
    conker::input::RumbleFeel hold;
    bool holding = false;
    if (!mounted) {
        g_was_mounted[0] = false;
        weapon_feel(rdram, player, 0, &effect, &rumble, &holding, &hold);
    }
    if (conker != 0 && !mounted && adaptive_triggers_enabled()) {
        const uint16_t animation = MEM_HU(kActorAnimation, kseg0(conker));
        if (animation == kPeeStart || animation == kPeeing) {
            if (const float power = pee_power(rdram, conker); power >= 0.0f) {
                effect = pee_effect(power);
            }
        }
    }
    conker::input::set_trigger_effect(effect);
    conker::input::set_rumble_feel(0, rumble);
    conker::input::set_hold_rumble(0, holding, hold);
    for (int i = 1; i < conker::input::kPorts; i++) {
        conker::input::set_trigger_effect(TriggerEffect{}, i);  // free, as a match may have left them
        g_was_mounted[i] = false;
    }
}

nlohmann::json conker::status::adaptive_triggers() {
    return enhancement("adaptive_triggers", "Peeing and the machine gun in the triggers", adaptive_triggers_enabled(),
                       "CONKER_ADAPTIVE_TRIGGERS", {{"pad_has_adaptive_triggers", conker::input::has_adaptive_triggers()},
                        {"pad_has_haptics", conker::input::has_haptics()}});
}

conker::switches::Switch conker::switches::adaptive_triggers() {
    // Each player's own online: it only sets this machine's pad, from game memory it never writes.
    return {"ADAPTIVE TRIGGERS", adaptive_triggers_enabled, [](bool on) { g_enabled.store(on); }, true};
}
