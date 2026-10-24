// Twin-stick controls while Conker holds the shotgun (Spooky's mansion, out with B): the left
// stick always moves him and the right stick aims. On the N64 the stick turns and walks, the C
// buttons step (C-up and C-down forwards and back, C-left and C-right sideways, Conker keeps
// facing the same way) and, with R held to aim, the stick aims instead, pushed up to aim down.
// So while he holds it:
//  * the right stick's C-left and C-right are not swapped, as they are for the camera elsewhere
//    (input::camera_swapped): pushed right, he steps right;
//  * with R held, the right stick is the stick, up aiming up, and the left stick is the C
//    buttons, so he still walks and steps with it while aiming. Controller 1 on the keyboard
//    alone keeps the game's aiming, as it has no right stick.
// Controller 1's pad as the game reads it is rewritten in func_1501BBB8 (sp+0x38, after
// osContGetReadData; the [[patches.hook]] in config/us.toml), so the game sees nothing else.
// CONKER_GUN_CONTROLS=0 leaves the game's controls.
//
// Found in scene 60 (debug report scene60-20261025-081607, B to take the shotgun out): the item
// Conker holds is u8 +0x78 of his player block (*(Conker's actor + 0x31C)), 0x3B the shotgun, and
// u8 +0x11A how far: 1 coming out, 2 held, 3 going away (func_15065A5C sets both). The block's
// u8 +0x198 is 1 then 2 while R aims, with or without the shotgun.
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>

#include "recomp.h"
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

constexpr int kPad = 0x38;  // func_1501BBB8's frame: controller 1's OSContPad
constexpr float kStickRange = 82.0f;  // the N64 stick's reach on an axis
constexpr int kStepAt = 41;           // the left stick, half way, steps as a C button would

using namespace conker::input;

std::atomic<bool> g_enabled{[] {
    const char* value = std::getenv("CONKER_GUN_CONTROLS");
    return value == nullptr || value[0] != '0';
}()};  // the debug menu switches it

bool gun_controls_enabled() { return g_enabled.load(std::memory_order_relaxed); }

bool in_rdram(uint32_t address) { return address >= 0x80000000u && address < 0x80800000u; }

bool holding_shotgun(uint8_t* rdram) {
    const uint32_t conker = static_cast<uint32_t>(MEM_W(0, kseg0(kConker)));
    if (!in_rdram(conker)) {
        return false;
    }
    const uint32_t player = static_cast<uint32_t>(MEM_W(kActorPlayer, kseg0(conker)));
    if (!in_rdram(player)) {
        return false;
    }
    return MEM_BU(kPlayerItem, kseg0(player)) == kShotgun && MEM_BU(kPlayerItemState, kseg0(player)) == kItemHeld;
}

int8_t stick_axis(float value) {
    return static_cast<int8_t>(std::lround(std::fmax(-1.0f, std::fmin(value, 1.0f)) * kStickRange));
}

}  // namespace

// In func_1501BBB8 just after the pads are read ($sp is its frame), before the debug menu empties
// them while it is open.
extern "C" void conker_gun_controls(uint8_t* rdram, recomp_context* ctx) {
    if (!gun_controls_enabled() || !holding_shotgun(rdram)) {
        return;
    }
    uint16_t buttons = MEM_HU(kPad, ctx->r29);
    float aim_x;
    float aim_y;
    if ((buttons & kR) != 0 && right_stick(&aim_x, &aim_y)) {
        const int walk_x = MEM_B(kPad + 2, ctx->r29);
        const int walk_y = MEM_B(kPad + 3, ctx->r29);
        buttons &= static_cast<uint16_t>(~(kCUp | kCDown | kCLeft | kCRight));
        if (walk_y > kStepAt) buttons |= kCUp;
        if (walk_y < -kStepAt) buttons |= kCDown;
        if (walk_x < -kStepAt) buttons |= kCLeft;
        if (walk_x > kStepAt) buttons |= kCRight;
        MEM_B(kPad + 2, ctx->r29) = stick_axis(aim_x);
        MEM_B(kPad + 3, ctx->r29) = stick_axis(-aim_y);
    } else if (camera_swapped()) {
        const uint16_t c = buttons & (kCLeft | kCRight);
        buttons = static_cast<uint16_t>((buttons & ~(kCLeft | kCRight)) | ((c & kCLeft) ? kCRight : 0) |
                                        ((c & kCRight) ? kCLeft : 0));
    }
    MEM_H(kPad, ctx->r29) = static_cast<int16_t>(buttons);
}

nlohmann::json conker::status::gun_controls() {
    return enhancement("gun_controls", "Twin-stick shotgun", gun_controls_enabled(), "CONKER_GUN_CONTROLS");
}

conker::switches::Switch conker::switches::gun_controls() {
    return {"TWIN STICK SHOTGUN", gun_controls_enabled, [](bool on) { g_enabled.store(on); }};
}
