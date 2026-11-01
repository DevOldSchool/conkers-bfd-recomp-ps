// Conker's projected shadow cast from overhead in the Heist intro, so it lies under his feet. Native helper
// called from the [[patches.hook]] snippet in config/us.toml (func_15186794).
//
// func_15186794 draws Conker's silhouette into a 64x64 8-bit buffer (0x800DE080) and lays it on the floor
// around him, projected from a light: func_15184368 picks the nearest point light from the scene's light list
// (D_800DCD78) and, when there is none, uses one 600 units above him (D_800DE010, set to (1, 600, -200)).
// In the Heist intro (scene 64) the nearest light sits low and off to one side, so the silhouette is thrown
// sideways and lands as a dark streak beside Conker. The console hides most of it: the camera sits low on the
// floor, the quads reach in front of the near plane, and the RSP's guard-band clipping and screen-Z clamp push
// them behind the floor. RT64 interpolates the true depth and shows it. Turning off the light search
// (D_800DE034, which only func_15184368 reads and only scene 33 sets) while the intro's cutscenes play casts
// the shadow from the overhead fallback instead.
#include <cstdint>

#include "recomp.h"

namespace {

inline gpr kseg0(uint32_t address) { return static_cast<gpr>(static_cast<int32_t>(address)); }

constexpr uint32_t kScene = 0x800BE9F0;            // D_800BE9F0: the scene being played
constexpr uint32_t kCutscenePlaying = 0x800C35EA;  // 1 while a cutscene script runs
constexpr uint32_t kNoPointLights = 0x800DE034;    // D_800DE034: nonzero to skip the light search
constexpr uint32_t kHeistScene = 64;

}  // namespace

// At func_15186794's entry, which draws the projected shadows. Scene 64 keeps the light search off (its
// default, from func_151872B0, is on) while a cutscene plays.
extern "C" void conker_projected_shadow_light(uint8_t* rdram) {
    if (MEM_W(0, kseg0(kScene)) != static_cast<int32_t>(kHeistScene)) {
        return;
    }
    MEM_W(0, kseg0(kNoPointLights)) = MEM_BU(0, kseg0(kCutscenePlaying)) == 1 ? 1 : 0;
}
