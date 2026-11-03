// Consistent controls: the same controls wherever Conker aims, as a modern pad's games have them,
// the left stick moving him, the right stick turning the view or aiming, and up looking up.
//
// The camera on the right stick: C-left and C-right are swapped (input::camera_swapped, also on
// J/L), so pushing the right stick left turns the view left, where the N64's C-left swings it right.
//
// Aiming the right way up. Holding R without the shotgun looks around in first person, and the
// stick, pushed up, looks down; it is turned round so up looks up. The slingshot (B on its pad)
// aims the same way without R, and gets the same, the mouse aiming it too.
//
// Twin-stick controls while Conker holds the shotgun (Spooky's mansion, out with B): the left
// stick always moves him and the right stick aims. On the N64 the stick turns and walks, the C
// buttons step (C-up and C-down forwards and back, C-left and C-right sideways, Conker keeps
// facing the same way) and, with R held to aim, the stick aims instead, pushed up to aim down.
// So while he holds it:
//  * the right stick's C-left and C-right are not swapped, as they are for the camera elsewhere
//    (input::camera_swapped, above): pushed right, he steps right;
//  * with R held, the right stick is the stick, up aiming up, and the left stick is the C
//    buttons, so he still walks and steps with it while aiming. Controller 1 on the keyboard
//    alone keeps the game's aiming, as it has no right stick.
//
// The rocket launcher in a multiplayer match (THE VAULT) scoped with R works the same way: the
// stick aims, up aiming down, and C-up and C-down walk. So while scoped it gets the shotgun's aim:
// the right stick aims, up aiming up, and the left stick walks. Unscoped it walks with the stick.
// It's War's rocket launcher (item 0x0C, from its B pads) scopes the same way and gets the same.
// The matches' scoped gun, item 0x24, scopes with R the same way and gets the same, as does
// anything else aimed through a sight there (the knives, item 0x14: +0x197 is 2 while R is held).
// Heist's B pads (Enter the Vertex, scene 54), the Uzis out: B jumps sideways in slow motion, and
// through the jump the stick moves the aimer, not Conker, whose path is set when he takes off. So
// through it the right stick (and the mouse) is the stick, as the left stick is: the same way up.
// The left stick and the right stick's C buttons do nothing then.
// The mounted machine guns (the wasps' hive, scene 39; It's War's, scene 38) aim with the stick,
// pushed up to aim down, as R's look does, and get the same: up aims up, the mouse aiming too.
// The tanks (TANK in MULTI, any player): C-left and C-right turn the turret, C-left to the left,
// so the camera's swap turned it the wrong way; a player in a tank gets them back the N64's way,
// the right stick pushed left turning the turret left, and none of the above, as a tank's player
// block can read as holding a gun. A player's tank in TANK (scene 36) is an actor of model 0x77
// (logged in a match); 0x28 is what Rare's func_15075548 tests for too. Player n is actor n and
// reads the pad D_80084060[n] names.
// The story's tank (Saving Private Rodent, scene 14) is actor 1, model 0x28, and Conker stays
// actor 0 inside it: the tank's u8 +0x13C is 100 + its rider's index (as func_150BDB70 reads a
// mounted gun's), 100 for Conker, so a player riding a tank gets the tank's controls too.
// Aiming its gun with R (+0x197 2 as on a mounted gun, item 0x1F), the stick aims, X turning the
// turret and, pushed up, aiming down, and the C buttons do nothing; there the right stick aims,
// up aiming up, as the shotgun's does.
// The matches' guns (picked up; TEMPLE's shotgun is item 0x37) handle as the story's shotgun
// does, so they get its controls too; every player in a match gets all of the above on their own
// pad, controller 1 alone the mouse.
// On foot in a match the camera is locked behind the player: its controls block's +0x84 has bit 21,
// so func_1512D390 does not swing it with C-left and C-right, which step the player sideways
// instead, and the camera eases back behind the facing (logged: turned with the mouse past the
// lock, it swung back within about 1.5 s). The left stick runs where the camera looks, so pushed
// sideways the player turns towards it and the camera follows, round in a circle. So there the
// right stick's X and the mouse turn the player (facing +0x40, +0x76, +0x7A of the actor) and the
// camera together (mouse_look::turn_camera), the view staying where they aim, and the left stick
// moves the player as the game has it (it turns with the stick pushed sideways and the camera
// follows, as on an N64); with the mouse captured, A and D step sideways as C-left and C-right do,
// as in a shooter. The right stick's C buttons are taken off: C-up steps forwards with a gun. R's look without a gun gets
// the gun's aim there, the right stick looking (up looks up) and the left stick the C buttons, so
// the camera never moves with the left stick (looking, the game does not walk with them).
// The pads as the game reads them are rewritten in func_1501BBB8 (sp+0x38, four OSContPads, after
// osContGetReadData; the [[patches.hook]] in config/us.toml), so the game sees nothing else.
// CONKER_CONSISTENT_CONTROLS=0 leaves the game's controls.
//
// Found in scene 60 (debug report scene60-20261025-081607, B to take the shotgun out): the item
// Conker holds is u8 +0x78 of his player block (*(Conker's actor + 0x31C)), 0x3B the shotgun, and
// u8 +0x11A how far: 1 coming out, 2 held, 3 going away (func_15065A5C sets both). The block's
// u8 +0x198 is 1 then 2 while R aims, with or without the shotgun, and 3 as it lets go (the
// camera's s16 +0x73C, copied there by func_1512317C, so the slingshot's aim sets it too); looking
// with it in scene 60, the stick pushed up turned the camera's forward vector (+0x130) down.
// The launcher was found by logging these through a VAULT match (scene 26): it is item
// 0x16, and u8 +0x197 is 2 while its scope is up (the mounted gun's "mounted" too).
// The slingshot by logging them in scene 6 (debug report scene6-20261023-133408, B on its pad):
// item 0x02, and +0x198 is 2 while it is out, R or not; the stick pushed up turned +0x130 down.
// The mounted gun by logging them in scene 38 (debug report scene38-20261027-213708, on the gun):
// +0x197 is 2 and the held item 0x1B; the stick pushed up turned +0x130 down and the gun's pitch
// (f32 +0x170, func_150BDB70 aims the gun with it) up to 50, its lowest.
// The Uzis' jump by logging them in scene 54 (debug report scene54-20261103-185231, on a B pad):
// the B pad's action is u8 +0x75 0x89, the Uzis item 0x09, and B plays animation 0x20F (0x20E the
// other way, func_15065A5C); the stick moved nothing but the aimer while it played.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "recomp.h"
#include "enhancements/consistent_controls/mouse_look.hpp"
#include "enhancements/status.hpp"
#include "enhancements/switches.hpp"
#include "platform/input.hpp"
#include "runtime/frame_input.hpp"

namespace {

inline gpr kseg0(uint32_t address) { return static_cast<gpr>(static_cast<int32_t>(address)); }

constexpr uint32_t kConker = 0x80088720;  // D_80088720: Conker's actor
constexpr uint32_t kActorPlayer = 0x31C;  // the player's block
constexpr uint32_t kPlayerItem = 0x78;    // u8: the item he holds
constexpr uint32_t kPlayerItemState = 0x11A;  // u8
// The guns that aim with R, as Rare lists them in func_15121490: 0x3B the story's shotgun, 0x37
// the matches' (TEMPLE), the rest the matches' other guns.
constexpr uint8_t kGuns[] = {0x09, 0x15, 0x26, 0x37, 0x38, 0x39, 0x3A, 0x3B};
constexpr uint8_t kItemHeld = 2;
constexpr uint8_t kUzis = 0x09;
constexpr uint8_t kRocketLauncher = 0x16;
constexpr uint8_t kStoryRocketLauncher = 0x0C;  // It's War's, from its B pads; scoped the same way
constexpr uint8_t kScopedGun = 0x24;  // a match's gun with a scope on R, as the launcher's
constexpr uint8_t kSlingshot = 0x02;  // out with B on its pad, aimed without R
constexpr uint32_t kPlayerScoped = 0x197;  // u8: 2 while the launcher's scope is up, or mounted
constexpr uint8_t kScoped = 2;
constexpr uint32_t kActorAnimation = 0x84;  // u16: the animation Conker plays
constexpr uint16_t kUziJumps[] = {0x20E, 0x20F};  // the B pads' jump with the Uzis, to his left and right
constexpr uint32_t kPlayerLook = 0x198;  // u8: 1 going into R's look, 2 looking, 3 coming out

constexpr uint32_t kActors = 0x800CC2D0;       // D_800CC2D0: the actors; player n is actor n in a match
constexpr uint32_t kActorSize = 0x32C;
constexpr uint32_t kActorModel = 0x4;          // u8
constexpr int kActorCount = 25;
constexpr uint8_t kTankModels[] = {0x28, 0x77};
constexpr uint32_t kActorRider = 0x13C;  // u8: the actor riding it, as kRiderBias + its index
constexpr int kRiderBias = 100;
constexpr uint32_t kMultiplayer = 0x800BE616;  // D_800BE616: nonzero during a multiplayer match
constexpr uint32_t kPlayerPads = 0x80084060;   // D_80084060: u8 per player, the pad it reads
constexpr uint32_t kFrameDelta = 0x800BE9A4;   // D_800BE9A4: this frame's logic step, 1.0 = 1/30 s
constexpr uint32_t kActorFacing = 0x40;        // f32: degrees from +Z towards +X
constexpr uint32_t kActorFacingAngle = 0x76;   // s16: the facing it is made from, and the one it
constexpr uint32_t kActorFacingTarget = 0x7A;  // turns towards (0x10000 a turn, 90 degrees behind)
constexpr float kMatchTurn = 6.0f;             // degrees a 1/30 s step, the right stick pushed fully

constexpr int kPad = 0x38;  // func_1501BBB8's frame: controller 1's OSContPad
constexpr int kPadSize = 6;  // OSContPad, four of them
constexpr float kStickRange = 82.0f;  // the N64 stick's reach on an axis
constexpr int kStepAt = 41;           // the left stick, half way, steps as a C button would

using namespace conker::input;

std::atomic<bool> g_enabled{[] {
    const char* value = std::getenv("CONKER_CONSISTENT_CONTROLS");
    return value == nullptr || value[0] != '0';
}()};  // the debug menu switches it

// The camera swap follows the switch from the start (input_sdl.cpp's flag is constant-initialized).
[[maybe_unused]] const bool g_camera_swap_applied = [] {
    set_camera_swapped(g_enabled.load());
    return true;
}();

bool consistent_controls_enabled() { return g_enabled.load(std::memory_order_relaxed); }

void set_consistent_controls(bool on) {
    g_enabled.store(on);
    set_camera_swapped(on);
}

bool in_rdram(uint32_t address) { return address >= 0x80000000u && address < 0x80800000u; }

int8_t stick_axis(float value) {
    return static_cast<int8_t>(std::lround(std::fmax(-1.0f, std::fmin(value, 1.0f)) * kStickRange));
}

int players(uint8_t* rdram) { return MEM_BU(0, kseg0(kMultiplayer)) != 0 ? 4 : 1; }

// The pad player n reads, or -1 for none (a match's bots). Outside a match only Conker, on
// controller 1.
int player_pad(uint8_t* rdram, int player) {
    if (MEM_BU(0, kseg0(kMultiplayer)) == 0) {
        return player == 0 ? 0 : -1;
    }
    const uint8_t pad = MEM_BU(player, kseg0(kPlayerPads));
    return pad < 4 ? pad : -1;
}

// Player n's actor: Conker's for the first.
uint32_t player_actor(uint8_t* rdram, int player) {
    return player == 0 ? static_cast<uint32_t>(MEM_W(0, kseg0(kConker))) : kActors + player * kActorSize;
}

// Player n's block, or 0.
uint32_t player_block(uint8_t* rdram, int player) {
    const uint32_t actor = player_actor(rdram, player);
    if (!in_rdram(actor)) {
        return 0;
    }
    const uint32_t block = static_cast<uint32_t>(MEM_W(kActorPlayer, kseg0(actor)));
    return in_rdram(block) ? block : 0;
}

bool holding(uint8_t* rdram, uint32_t player, uint8_t item) {
    return MEM_BU(kPlayerItem, kseg0(player)) == item && MEM_BU(kPlayerItemState, kseg0(player)) == kItemHeld;
}

bool holding_gun(uint8_t* rdram, uint32_t player) {
    return std::any_of(std::begin(kGuns), std::end(kGuns), [&](uint8_t gun) { return holding(rdram, player, gun); });
}

bool launcher_scoped(uint8_t* rdram, uint32_t player) {
    return (holding(rdram, player, kRocketLauncher) || holding(rdram, player, kStoryRocketLauncher) ||
            holding(rdram, player, kScopedGun)) &&
           MEM_BU(kPlayerScoped, kseg0(player)) == kScoped;
}

float read_float(uint8_t* rdram, uint32_t address) {
    const uint32_t word = static_cast<uint32_t>(MEM_W(0, kseg0(address)));
    float value;
    std::memcpy(&value, &word, sizeof(value));
    return value;
}

void write_float(uint8_t* rdram, uint32_t address, float value) {
    uint32_t word;
    std::memcpy(&word, &value, sizeof(word));
    MEM_W(0, kseg0(address)) = static_cast<int32_t>(word);
}

// The mouse's aim, the pad's port's mouse; false when it has none (not captured).
bool mouse_aim(int pad, float* x, float* y) {
    return conker::mouse_look::take_aim(pad, x, y);
}

// Aims with the right stick (and the port's mouse), up aiming up, and walks with the left
// stick (or WASD) as the C buttons, on the pad at `at` in the frame. False, leaving the pad
// alone, when its controller has neither (the keyboard alone, the mouse not captured).
bool twin_stick_aim(uint8_t* rdram, recomp_context* ctx, int pad, int at, uint16_t& buttons) {
    float aim_x;
    float aim_y;
    float mouse_x;
    float mouse_y;
    const bool stick = conker::frame_input::right_stick(&aim_x, &aim_y, pad);
    if (!mouse_aim(pad, &mouse_x, &mouse_y) && !stick) {
        return false;
    }
    aim_x += mouse_x;
    aim_y += mouse_y;
    const int walk_x = MEM_B(at + 2, ctx->r29);
    const int walk_y = MEM_B(at + 3, ctx->r29);
    buttons &= static_cast<uint16_t>(~(kCUp | kCDown | kCLeft | kCRight));
    if (walk_y > kStepAt) buttons |= kCUp;
    if (walk_y < -kStepAt) buttons |= kCDown;
    if (walk_x < -kStepAt) buttons |= kCLeft;
    if (walk_x > kStepAt) buttons |= kCRight;
    MEM_B(at + 2, ctx->r29) = stick_axis(aim_x);
    MEM_B(at + 3, ctx->r29) = stick_axis(-aim_y);
    return true;
}

uint16_t swap_c_left_right(uint16_t buttons) {
    const uint16_t c = buttons & (kCLeft | kCRight);
    return static_cast<uint16_t>((buttons & ~(kCLeft | kCRight)) | ((c & kCLeft) ? kCRight : 0) |
                                 ((c & kCRight) ? kCLeft : 0));
}

bool tank(uint8_t* rdram, uint32_t actor) {
    const uint8_t model = MEM_BU(kActorModel, kseg0(actor));
    return std::find(std::begin(kTankModels), std::end(kTankModels), model) != std::end(kTankModels);
}

// Whether player n drives a tank: is one (a match's) or rides one (the story's).
bool in_tank(uint8_t* rdram, int player) {
    if (tank(rdram, player_actor(rdram, player))) {
        return true;
    }
    for (int i = 0; i < kActorCount; i++) {
        const uint32_t actor = kActors + i * kActorSize;
        if (MEM_BU(kActorRider, kseg0(actor)) == kRiderBias + player && tank(rdram, actor)) {
            return true;
        }
    }
    return false;
}

bool looking(uint8_t* rdram, uint32_t player) {
    const uint8_t look = MEM_BU(kPlayerLook, kseg0(player));
    return look == 1 || look == 2;
}

// On a mounted gun (the binocular frame up): u8 +0x197 is 2 there too, whatever he held.
bool mounted(uint8_t* rdram, uint32_t player) { return MEM_BU(kPlayerScoped, kseg0(player)) == kScoped; }

// Aiming through a sight: the camera's sight mode (func_15128CB0) copies its state into u8 +0x197
// for whatever is held, and clears it when nothing is.
bool sighted(uint8_t* rdram, uint32_t player) { return MEM_BU(kPlayerScoped, kseg0(player)) != 0; }

// The stick, where the game aims down with it pushed up, turned round to aim up; the mouse adds
// to it.
void aim_upright(uint8_t* rdram, recomp_context* ctx, int pad, int at) {
    float mouse_x;
    float mouse_y;
    mouse_aim(pad, &mouse_x, &mouse_y);
    const float x = MEM_B(at + 2, ctx->r29) / kStickRange + mouse_x;
    const float y = -MEM_B(at + 3, ctx->r29) / kStickRange - mouse_y;
    MEM_B(at + 2, ctx->r29) = stick_axis(x);
    MEM_B(at + 3, ctx->r29) = stick_axis(y);
}

// A match on foot: the right stick's X (and the port's mouse) turns the player and the camera
// together; the left stick moves the player as the game has it, but for A and D stepping sideways
// while the mouse is captured. The right stick's C buttons are taken off the pad (the keyboard's
// I, J, K and L stay); D-pad down and Triangle give C-down, the grenade. False, leaving the pad alone, when the
// controller has neither a right stick nor a captured mouse (the keyboard alone).
bool match_look(uint8_t* rdram, recomp_context* ctx, uint32_t actor, int pad, int at, uint16_t& buttons) {
    float right_x;
    float right_y;
    const bool stick = conker::frame_input::right_stick(&right_x, &right_y, pad);
    // Paused (or in a menu) the pad is left alone: the pause menu is steered with the stick.
    if ((!stick && !conker::frame_input::mouse_captured(pad)) || !conker::mouse_look::playing()) {
        return false;
    }
    if (stick) {
        buttons &= static_cast<uint16_t>(~(kCUp | kCDown | kCLeft | kCRight));
    }
    // C-down throws a grenade; with the right stick taken it is D-pad down or Triangle.
    if ((buttons & (kDDown | kExtra)) != 0) {
        buttons |= kCDown;
    }
    // Aiming with the mouse, A and D (the stick's X) step sideways as C-left and C-right do, as
    // in a shooter; on a pad the left stick moves the player as the game has it.
    if (conker::frame_input::mouse_captured(pad)) {
        const int walk_x = MEM_B(at + 2, ctx->r29);
        if (walk_x < -kStepAt) buttons |= kCLeft;
        if (walk_x > kStepAt) buttons |= kCRight;
        MEM_B(at + 2, ctx->r29) = 0;
    }
    // Squared, for small turns near the middle.
    const float turn = right_x * std::fabs(right_x) * kMatchTurn * read_float(rdram, kFrameDelta) +
                       conker::mouse_look::take_turn(pad);
    if (turn == 0.0f) {
        return true;
    }
    // Turned right, the facing goes down.
    write_float(rdram, actor + kActorFacing, read_float(rdram, actor + kActorFacing) - turn);
    const auto angle = static_cast<int16_t>(std::lround(-turn * (65536.0f / 360.0f)));
    MEM_H(kActorFacingAngle, kseg0(actor)) = static_cast<int16_t>(MEM_H(kActorFacingAngle, kseg0(actor)) + angle);
    MEM_H(kActorFacingTarget, kseg0(actor)) = static_cast<int16_t>(MEM_H(kActorFacingTarget, kseg0(actor)) + angle);
    conker::mouse_look::turn_camera(pad, turn);
    return true;
}

bool uzi_jumping(uint8_t* rdram, uint32_t actor) {
    const uint16_t animation = MEM_HU(kActorAnimation, kseg0(actor));
    return std::find(std::begin(kUziJumps), std::end(kUziJumps), animation) != std::end(kUziJumps);
}

// The right stick (and the port's mouse) as the stick, the same way up, the C buttons off. False,
// leaving the pad alone, when the controller has neither.
bool right_stick_aim(uint8_t* rdram, recomp_context* ctx, int pad, int at, uint16_t& buttons) {
    float aim_x;
    float aim_y;
    float mouse_x;
    float mouse_y;
    const bool stick = conker::frame_input::right_stick(&aim_x, &aim_y, pad);
    if (!mouse_aim(pad, &mouse_x, &mouse_y) && !stick) {
        return false;
    }
    buttons &= static_cast<uint16_t>(~(kCUp | kCDown | kCLeft | kCRight));
    MEM_B(at + 2, ctx->r29) = stick_axis(aim_x + mouse_x);
    MEM_B(at + 3, ctx->r29) = stick_axis(aim_y + mouse_y);
    return true;
}

// Rewrites the pad of the player whose block this is, read from controller `pad`.
void remap(uint8_t* rdram, recomp_context* ctx, uint32_t actor, uint32_t player, int pad) {
    const int at = kPad + pad * kPadSize;
    uint16_t buttons = MEM_HU(at, ctx->r29);
    const bool match = MEM_BU(0, kseg0(kMultiplayer)) != 0;
    if (!match && holding(rdram, player, kUzis) && uzi_jumping(rdram, actor)) {
        if (right_stick_aim(rdram, ctx, pad, at, buttons)) {
            MEM_H(at, ctx->r29) = static_cast<int16_t>(buttons);
        }
        return;
    }
    // In a match whatever aims with a sight (the knives, item 0x14, as much as the launcher) aims
    // as the launcher's scope does, a match's mounted gun (item 0x1B) too; tanks never get here.
    if (launcher_scoped(rdram, player) || (match && sighted(rdram, player))) {
        if (twin_stick_aim(rdram, ctx, pad, at, buttons)) {
            MEM_H(at, ctx->r29) = static_cast<int16_t>(buttons);
        }
        return;
    }
    if (mounted(rdram, player)) {
        aim_upright(rdram, ctx, pad, at);
        return;
    }
    if (!holding_gun(rdram, player)) {
        const bool aiming = (buttons & kR) != 0 || holding(rdram, player, kSlingshot);
        if (aiming && looking(rdram, player)) {
            // In a match the right stick aims and the left stick walks, as with a gun.
            if (match && twin_stick_aim(rdram, ctx, pad, at, buttons)) {
                MEM_H(at, ctx->r29) = static_cast<int16_t>(buttons);
            } else {
                aim_upright(rdram, ctx, pad, at);
            }
        } else if (match && (buttons & kR) == 0 && match_look(rdram, ctx, actor, pad, at, buttons)) {
            MEM_H(at, ctx->r29) = static_cast<int16_t>(buttons);
        }
        return;
    }
    const bool aimed = (buttons & kR) != 0 && twin_stick_aim(rdram, ctx, pad, at, buttons);
    if (!aimed && match && (buttons & kR) == 0 && match_look(rdram, ctx, actor, pad, at, buttons)) {
        MEM_H(at, ctx->r29) = static_cast<int16_t>(buttons);
        return;
    }
    if (!aimed && camera_swapped()) {
        buttons = swap_c_left_right(buttons);
    }
    MEM_H(at, ctx->r29) = static_cast<int16_t>(buttons);
}

}  // namespace

// In func_1501BBB8 just after the pads are read ($sp is its frame), before the debug menu empties
// them while it is open.
extern "C" void conker_consistent_controls(uint8_t* rdram, recomp_context* ctx) {
    conker::mouse_look::frame(rdram);
    if (!consistent_controls_enabled()) {
        return;
    }
    for (int player = 0; player < players(rdram); player++) {
        const int pad = player_pad(rdram, player);
        if (pad >= 0 && in_tank(rdram, player)) {
            const int at = kPad + pad * kPadSize;
            const uint32_t block = player_block(rdram, player);
            if (block != 0 && mounted(rdram, block)) {
                // Aiming (R): the right stick aims, up aiming up, or the stick turned upright.
                uint16_t buttons = MEM_HU(at, ctx->r29);
                if (twin_stick_aim(rdram, ctx, pad, at, buttons)) {
                    MEM_H(at, ctx->r29) = static_cast<int16_t>(buttons);
                } else {
                    aim_upright(rdram, ctx, pad, at);
                }
                continue;
            }
            if (camera_swapped()) {
                MEM_H(at, ctx->r29) = static_cast<int16_t>(swap_c_left_right(MEM_HU(at, ctx->r29)));
            }
            continue;
        }
        const uint32_t block = pad >= 0 ? player_block(rdram, player) : 0;
        if (block != 0) {
            remap(rdram, ctx, player_actor(rdram, player), block, pad);
        }
    }
}

nlohmann::json conker::status::consistent_controls() {
    return enhancement("consistent_controls", "Consistent controls", consistent_controls_enabled(),
                       "CONKER_CONSISTENT_CONTROLS");
}

conker::switches::Switch conker::switches::consistent_controls() {
    return {"CONSISTENT CONTROLS", consistent_controls_enabled, set_consistent_controls};
}
