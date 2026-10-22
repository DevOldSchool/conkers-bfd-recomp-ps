// Characters drawn as far as the scene is. Native helper called from the [[patches.hook]] snippet
// in config/us.toml (func_1502E4C4). CONKER_CHARACTER_DISTANCE=0 keeps the game's distances.
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "recomp.h"
#include "enhancements/status.hpp"
#include "enhancements/switches.hpp"

namespace {

constexpr uint32_t kSceneFar = 0x800D9B1C;  // float, the scene's far distance (fog, far plane)

inline gpr kseg0(uint32_t address) { return static_cast<gpr>(static_cast<int32_t>(address)); }

float read_float(uint8_t* rdram, uint32_t address) {
    const uint32_t bits = MEM_W(0, kseg0(address));
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

std::atomic<bool> g_enabled{[] {
    const char* value = std::getenv("CONKER_CHARACTER_DISTANCE");
    return value == nullptr || value[0] != '0';
}()};  // the debug menu switches it

bool character_distance_enabled() { return g_enabled.load(std::memory_order_relaxed); }

}  // namespace

// func_1502E4C4 decides, for each view, whether an actor is drawn. Past its own distance from the
// camera, u16 actor +0x10 (3000 for most, up to 7000), the actor is faded out (func_1506160C code
// 8, its per-view opacity at +0xB going to 0) and left out once the opacity is 0; nearer, it fades
// back in (code 9). A distance of 0 means always drawn. Only the opacity changes: the actor's
// script and animation run either way. The scene itself reaches much farther (13000 in scene 12),
// so a character in plain view stood missing until the camera came closer (scene 12's sunflower
// on the ledge past the stone steps, 3000).
//
// Called at 0x1502E770 with the distance in $s0, where the game has already put 100000 for a
// camera that shows everything: a nonzero distance under the scene's far distance becomes it.
// Drawing more actors cannot overflow the display list, since func_1502C974 drops an actor that
// would take the list past its limit.
extern "C" void conker_character_distance(uint8_t* rdram, recomp_context* ctx) {
    if (!character_distance_enabled()) {
        return;
    }
    const int32_t distance = static_cast<int32_t>(ctx->r16);
    const float far_distance = read_float(rdram, kSceneFar);
    if (distance > 0 && far_distance > static_cast<float>(distance) && far_distance < 1.0e6f) {
        ctx->r16 = static_cast<int32_t>(far_distance);
    }
}

// Swarms of bees (see kSwarms in pause_save/resume_point.cpp) are drawn with their bees only while
// func_151787AC finds them in a view, within D_800A71E4 (2500) of the camera, so a swarm settled
// around scene 12's sunflower vanished from across the clearing. Called at 0x15178934 with that
// distance just loaded into $f22: it becomes the scene's far distance, as for the characters.
extern "C" void conker_swarm_distance(uint8_t* rdram, recomp_context* ctx) {
    if (!character_distance_enabled()) {
        return;
    }
    const float far_distance = read_float(rdram, kSceneFar);
    if (far_distance > ctx->f22.fl && far_distance < 1.0e6f) {
        ctx->f22.fl = far_distance;
    }
}

nlohmann::json conker::status::character_distance() {
    return enhancement("character_distance", "Characters seen from farther", character_distance_enabled(),
                       "CONKER_CHARACTER_DISTANCE");
}

conker::switches::Switch conker::switches::character_distance() {
    return {"CHARACTER DISTANCE", character_distance_enabled, [](bool on) { g_enabled.store(on); }};
}
