// Characters always drawn with their most detailed model. Native helper called from the
// [[patches.hook]] snippet in config/us.toml (func_1502C6E8). CONKER_MAX_DETAIL=0 keeps the game's
// distance-based choice.
#include <cstdint>
#include <cstdlib>

#include "recomp.h"

namespace {

bool max_detail_enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("CONKER_MAX_DETAIL");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}

}  // namespace

// func_1502C6E8 picks which of an actor's models to draw (its list at +0x2C4, ordinal 0 the most
// detailed) from the squared distance to the camera: under 500 units ordinal 0, then 730, 1500 and
// 2000 (D_80096DE0/D_80096DE4 hold the middle two) for 1 to 4, one less for actors whose float at
// +0x3C is under 3. Called at 0x1502C8C4 with that ordinal in $v1: it becomes 0, as for an actor
// beside the camera. The game's own adjustments after this point are kept: model 0x5A never uses
// ordinal 0, base model 0 uses ordinal 1 while D_800BE616 is set, and the ordinal is clamped to
// the actor's count of automatic models.
extern "C" void conker_max_detail(uint8_t* /*rdram*/, recomp_context* ctx) {
    if (max_detail_enabled()) {
        ctx->r3 = 0;
    }
}
