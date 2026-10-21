// What the game culls to its 4:3 view, culled to the widened view instead. Native helpers called
// from the [[patches.hook]] snippets in config/us.toml (func_1501B22C, func_1510B958).
//
// Widened, the picture showed holes at its sides where the game had left out what lay past the
// edges of the 4:3 view: the green of the never-cleared frame buffer, or whatever was drawn there
// before (scene 41, looking down from the ledge at the fenced yard). The game has two view tests,
// both worked out from a camera's horizontal field of view in degrees (camera struct
// *D_800BE628 + 0x180 * index, +0x74; the vertical one is +0x78):
//
// * func_1501B22C builds the left and right view planes (+0x88..+0x9C) from it, and the top and
//   bottom ones (+0xA4..+0xB4) from the vertical one. func_150A6360, a sphere-in-view test for
//   props and actors, is the only reader of those planes.
// * func_1510B958 turns it into an x scale, D_800D35E0 (D_800D35E4 for y), that func_150A6210,
//   func_150A5378 and func_150A50C0 multiply a view-space x by before comparing it with the depth.
//   This is the test that left out the level geometry.
//
// RT64 widens the view at the same height, so the vertical field of view is kept and the
// horizontal one is opened to the picture's rendered shape: tan(h/2) grows, and the x scale
// shrinks, by the ratio of the rendered shape to the picture's 292:216. A little more is added for
// the tests' coarse treatment of big spheres near the edges; culling a little wider than needed
// only draws a little more. In 4:3 the game's tests are left as they are.
// CONKER_WIDESCREEN_CULLING=0 keeps the 4:3 culling.
#include <cmath>
#include <cstdint>
#include <cstdlib>

#include "recomp.h"
#include "platform/display_mode.hpp"

namespace {

constexpr float kPictureAspect = float(conker::display::kPictureWidth) / conker::display::kPictureHeight;
constexpr float kMargin = 1.05f;
constexpr float kDegrees = 3.14159265358979f / 180.0f;
constexpr float kMaxFov = 170.0f;  // the planes stay in front of the camera

bool culling_enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("CONKER_WIDESCREEN_CULLING");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}

}  // namespace

// At 0x1501B264 in func_1501B22C, with the camera's horizontal field of view (degrees, from +0x74)
// loaded in $f4 just before it is halved and turned into the left and right planes' angle.
extern "C" void conker_widescreen_culling(uint8_t* /*rdram*/, recomp_context* ctx) {
    const float widening = conker::display::rendered_aspect() / kPictureAspect;
    if (widening <= 1.0f || !culling_enabled()) {
        return;
    }
    const float fov = ctx->f4.fl;
    if (!(fov > 0.0f && fov < kMaxFov)) {
        return;
    }
    const float wide = 2.0f * std::atan(std::tan(fov * 0.5f * kDegrees) * widening * kMargin) / kDegrees;
    ctx->f4.fl = wide < kMaxFov ? wide : kMaxFov;
}

// At 0x1510B9A0 in func_1510B958, with the x scale in $f4 just before it is stored to D_800D35E0.
extern "C" void conker_widescreen_culling_scale(uint8_t* /*rdram*/, recomp_context* ctx) {
    const float widening = conker::display::rendered_aspect() / kPictureAspect;
    if (widening <= 1.0f || !culling_enabled()) {
        return;
    }
    ctx->f4.fl /= widening * kMargin;
}
