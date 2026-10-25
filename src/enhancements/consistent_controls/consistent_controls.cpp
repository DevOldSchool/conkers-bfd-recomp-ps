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
// Controller 1's pad as the game reads it is rewritten in func_1501BBB8 (sp+0x38, after
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
constexpr uint8_t kShotgun = 0x3B;
constexpr uint8_t kItemHeld = 2;
constexpr uint8_t kRocketLauncher = 0x16;
constexpr uint32_t kPlayerScoped = 0x197;  // u8: 2 while the launcher's scope is up
constexpr uint8_t kScoped = 2;
constexpr uint32_t kPlayerLook = 0x198;  // u8: 1 going into R's look, 2 looking, 3 coming out

constexpr int kPad = 0x38;  // func_1501BBB8's frame: controller 1's OSContPad
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

// Conker's player block, or 0.
uint32_t player_block(uint8_t* rdram) {
    const uint32_t conker = static_cast<uint32_t>(MEM_W(0, kseg0(kConker)));
    if (!in_rdram(conker)) {
        return 0;
    }
    const uint32_t player = static_cast<uint32_t>(MEM_W(kActorPlayer, kseg0(conker)));
    return in_rdram(player) ? player : 0;
}

bool holding(uint8_t* rdram, uint32_t player, uint8_t item) {
    return MEM_BU(kPlayerItem, kseg0(player)) == item && MEM_BU(kPlayerItemState, kseg0(player)) == kItemHeld;
}

bool launcher_scoped(uint8_t* rdram, uint32_t player) {
    return holding(rdram, player, kRocketLauncher) && MEM_BU(kPlayerScoped, kseg0(player)) == kScoped;
}

// Aims with the right stick and the mouse, up aiming up, and walks with the left stick (or WASD)
// as the C buttons. False, leaving the pad alone, when controller 1 has neither (the keyboard
// alone, the mouse not captured).
bool twin_stick_aim(uint8_t* rdram, recomp_context* ctx, uint16_t& buttons) {
    float aim_x;
    float aim_y;
    float mouse_x;
    float mouse_y;
    const bool pad = right_stick(&aim_x, &aim_y);
    if (!conker::mouse_look::take_aim(&mouse_x, &mouse_y) && !pad) {
        return false;
    }
    aim_x += mouse_x;
    aim_y += mouse_y;
    const int walk_x = MEM_B(kPad + 2, ctx->r29);
    const int walk_y = MEM_B(kPad + 3, ctx->r29);
    buttons &= static_cast<uint16_t>(~(kCUp | kCDown | kCLeft | kCRight));
    if (walk_y > kStepAt) buttons |= kCUp;
    if (walk_y < -kStepAt) buttons |= kCDown;
    if (walk_x < -kStepAt) buttons |= kCLeft;
    if (walk_x > kStepAt) buttons |= kCRight;
    MEM_B(kPad + 2, ctx->r29) = stick_axis(aim_x);
    MEM_B(kPad + 3, ctx->r29) = stick_axis(-aim_y);
    return true;
}

bool looking(uint8_t* rdram, uint32_t player) {
    const uint8_t look = MEM_BU(kPlayerLook, kseg0(player));
    return look == 1 || look == 2;
}

}  // namespace

// In func_1501BBB8 just after the pads are read ($sp is its frame), before the debug menu empties
// them while it is open.
extern "C" void conker_consistent_controls(uint8_t* rdram, recomp_context* ctx) {
    conker::mouse_look::frame(rdram);
    const uint32_t player = consistent_controls_enabled() ? player_block(rdram) : 0;
    if (player == 0) {
        return;
    }
    uint16_t buttons = MEM_HU(kPad, ctx->r29);
    if (launcher_scoped(rdram, player)) {
        if (twin_stick_aim(rdram, ctx, buttons)) {
            MEM_H(kPad, ctx->r29) = static_cast<int16_t>(buttons);
        }
        return;
    }
    if (!holding(rdram, player, kShotgun)) {
        if ((buttons & kR) != 0 && looking(rdram, player)) {
            // The game looks down with the stick pushed up; the mouse adds to it.
            float mouse_x;
            float mouse_y;
            conker::mouse_look::take_aim(&mouse_x, &mouse_y);
            const float x = MEM_B(kPad + 2, ctx->r29) / kStickRange + mouse_x;
            const float y = -MEM_B(kPad + 3, ctx->r29) / kStickRange - mouse_y;
            MEM_B(kPad + 2, ctx->r29) = stick_axis(x);
            MEM_B(kPad + 3, ctx->r29) = stick_axis(y);
        }
        return;
    }
    const bool aimed = (buttons & kR) != 0 && twin_stick_aim(rdram, ctx, buttons);
    if (!aimed && camera_swapped()) {
        const uint16_t c = buttons & (kCLeft | kCRight);
        buttons = static_cast<uint16_t>((buttons & ~(kCLeft | kCRight)) | ((c & kCLeft) ? kCRight : 0) |
                                        ((c & kCRight) ? kCLeft : 0));
    }
    MEM_H(kPad, ctx->r29) = static_cast<int16_t>(buttons);
}

nlohmann::json conker::status::consistent_controls() {
    return enhancement("consistent_controls", "Consistent controls", consistent_controls_enabled(),
                       "CONKER_CONSISTENT_CONTROLS");
}

conker::switches::Switch conker::switches::consistent_controls() {
    return {"CONSISTENT CONTROLS", consistent_controls_enabled, set_consistent_controls};
}
