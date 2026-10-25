// Consistent controls: the same controls wherever Conker aims, as a modern pad's games have them,
// the left stick moving him, the right stick turning the view or aiming, and up looking up.
//
// The camera on the right stick: C-left and C-right are swapped (input::camera_swapped, also on
// J/L), so pushing the right stick left turns the view left, where the N64's C-left swings it right.
//
// Aiming the right way up. Holding R without the shotgun looks around in first person, and the
// stick, pushed up, looks down; it is turned round so up looks up.
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
// The matches' scoped gun, item 0x24, scopes with R the same way and gets the same.
// The matches' guns (picked up; TEMPLE's shotgun is item 0x37) handle as the story's shotgun
// does, so they get its controls too; every player in a match gets all of the above on their own
// pad (player n is actor n and reads the pad D_80084060[n] names), controller 1 alone the mouse.
// The pads as the game reads them are rewritten in func_1501BBB8 (sp+0x38, four OSContPads, after
// osContGetReadData; the [[patches.hook]] in config/us.toml), so the game sees nothing else.
// CONKER_CONSISTENT_CONTROLS=0 leaves the game's controls.
//
// Found in scene 60 (debug report scene60-20261025-081607, B to take the shotgun out): the item
// Conker holds is u8 +0x78 of his player block (*(Conker's actor + 0x31C)), 0x3B the shotgun, and
// u8 +0x11A how far: 1 coming out, 2 held, 3 going away (func_15065A5C sets both). The block's
// u8 +0x198 is 1 then 2 while R aims, with or without the shotgun, and 3 as it lets go; looking
// with it in scene 60, the stick pushed up turned the camera's forward vector (+0x130) down.
// The launcher was found by logging these through a VAULT match (scene 26): it is item
// 0x16, and u8 +0x197 is 2 while its scope is up (the mounted gun's "mounted" too).
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>

#include "recomp.h"
#include "enhancements/consistent_controls/mouse_look.hpp"
#include "enhancements/status.hpp"
#include "enhancements/switches.hpp"
#include "platform/input.hpp"

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
constexpr uint8_t kRocketLauncher = 0x16;
constexpr uint8_t kScopedGun = 0x24;  // a match's gun with a scope on R, as the launcher's
constexpr uint32_t kPlayerScoped = 0x197;  // u8: 2 while the launcher's scope is up
constexpr uint8_t kScoped = 2;
constexpr uint32_t kPlayerLook = 0x198;  // u8: 1 going into R's look, 2 looking, 3 coming out

constexpr uint32_t kActors = 0x800CC2D0;       // D_800CC2D0: the actors; player n is actor n in a match
constexpr uint32_t kActorSize = 0x32C;
constexpr uint32_t kMultiplayer = 0x800BE616;  // D_800BE616: nonzero during a multiplayer match
constexpr uint32_t kPlayerPads = 0x80084060;   // D_80084060: u8 per player, the pad it reads

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
    return (holding(rdram, player, kRocketLauncher) || holding(rdram, player, kScopedGun)) &&
           MEM_BU(kPlayerScoped, kseg0(player)) == kScoped;
}

// The mouse's aim, controller 1's alone; false when it has none (not captured).
bool mouse_aim(int pad, float* x, float* y) {
    *x = 0.0f;
    *y = 0.0f;
    return pad == 0 && conker::mouse_look::take_aim(x, y);
}

// Aims with the right stick (and controller 1's mouse), up aiming up, and walks with the left
// stick (or WASD) as the C buttons, on the pad at `at` in the frame. False, leaving the pad
// alone, when its controller has neither (the keyboard alone, the mouse not captured).
bool twin_stick_aim(uint8_t* rdram, recomp_context* ctx, int pad, int at, uint16_t& buttons) {
    float aim_x;
    float aim_y;
    float mouse_x;
    float mouse_y;
    const bool stick = right_stick(&aim_x, &aim_y, pad);
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

bool looking(uint8_t* rdram, uint32_t player) {
    const uint8_t look = MEM_BU(kPlayerLook, kseg0(player));
    return look == 1 || look == 2;
}

// Rewrites the pad of the player whose block this is, read from controller `pad`.
void remap(uint8_t* rdram, recomp_context* ctx, uint32_t player, int pad) {
    const int at = kPad + pad * kPadSize;
    uint16_t buttons = MEM_HU(at, ctx->r29);
    if (launcher_scoped(rdram, player)) {
        if (twin_stick_aim(rdram, ctx, pad, at, buttons)) {
            MEM_H(at, ctx->r29) = static_cast<int16_t>(buttons);
        }
        return;
    }
    if (!holding_gun(rdram, player)) {
        if ((buttons & kR) != 0 && looking(rdram, player)) {
            // The game looks down with the stick pushed up; the mouse adds to it.
            float mouse_x;
            float mouse_y;
            mouse_aim(pad, &mouse_x, &mouse_y);
            const float x = MEM_B(at + 2, ctx->r29) / kStickRange + mouse_x;
            const float y = -MEM_B(at + 3, ctx->r29) / kStickRange - mouse_y;
            MEM_B(at + 2, ctx->r29) = stick_axis(x);
            MEM_B(at + 3, ctx->r29) = stick_axis(y);
        }
        return;
    }
    const bool aimed = (buttons & kR) != 0 && twin_stick_aim(rdram, ctx, pad, at, buttons);
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
        const uint32_t block = pad >= 0 ? player_block(rdram, player) : 0;
        if (block != 0) {
            remap(rdram, ctx, block, pad);
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
