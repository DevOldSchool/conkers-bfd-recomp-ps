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
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>

#include "recomp.h"
#include "enhancements/switches.hpp"
#include "platform/display_mode.hpp"

namespace {

constexpr float kPictureAspect = float(conker::display::kPictureWidth) / conker::display::kPictureHeight;
constexpr float kMargin = 1.05f;
constexpr float kDegrees = 3.14159265358979f / 180.0f;
constexpr float kMaxFov = 170.0f;  // the planes stay in front of the camera

std::atomic<bool> g_culling{[] {
    const char* value = std::getenv("CONKER_WIDESCREEN_CULLING");
    return value == nullptr || value[0] != '0';
}()};  // the debug menu switches it

bool culling_enabled() { return g_culling.load(std::memory_order_relaxed); }

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

// The tiled sky: func_15110CFC puts four vertices in a frame's vertex buffer (D_800DBE88) for each
// tile that passes func_150A6210, the x-scale test above. func_15000AD0 gives each buffer room for
// 0x130 vertices (0x1C0 with two views), enough for the tiles a 4:3 view keeps; widened, more pass,
// and the overflow ran into the next heap block (scene 6, entered from scene 41's tunnel, crashed in
// func_80004250 a frame later). Scene 6 measured 272 vertices in 4:3 and 316 in 16:9. With the
// wider culling the buffers get room for kSkyVertices, enough for a picture past 32:9, and a tile
// that would still not fit is left out, so a window of any shape cannot overflow them.
namespace {
constexpr int32_t kSkyVertices = 0x400;
int32_t g_sky_capacity = 0;  // vertices per buffer, 0 when this scene has no tiled sky
}  // namespace

// At the start of func_15000AD0, which sets up a scene's sky.
extern "C" void conker_sky_vertices_reset(uint8_t* /*rdram*/, recomp_context* /*ctx*/) {
    g_sky_capacity = 0;
}

// At 0x15000B6C in func_15000AD0, with the vertices per buffer in $a0 before both buffers of each
// view are allocated (one block of $a0 * 32 bytes).
extern "C" void conker_sky_vertices(uint8_t* /*rdram*/, recomp_context* ctx) {
    int32_t vertices = static_cast<int32_t>(ctx->r4);
    if (culling_enabled() && vertices < kSkyVertices) {
        vertices = kSkyVertices;
        ctx->r4 = vertices;
    }
    g_sky_capacity = vertices;
}

// At 0x15111128 in func_15110CFC, after func_150A6210 ($v0, nonzero when the tile is in view). The
// buffer holds the vertices flushed so far (sp+0xD8) and the ones not yet flushed ($t4).
extern "C" void conker_sky_tile_fits(uint8_t* rdram, recomp_context* ctx) {
    if (ctx->r2 == 0 || g_sky_capacity == 0) {
        return;
    }
    const int32_t used = MEM_W(0xD8, ctx->r29) + static_cast<int32_t>(ctx->r12);
    if (used + 4 > g_sky_capacity) {
        ctx->r2 = 0;
    }
}

// The scenery sprites: func_1517DE5C has func_1517BBAC build a camera-facing quad for each sprite
// (sunflowers, grass, mushrooms) within a distance of Conker, 1500, with its vertex alpha fading
// over the last 510 (min(255, (distance limit - distance) / 2)). That is far enough for the 4:3
// view; widened, the picture's sides show ground farther from Conker, and the sprites there were
// missing or still faint (scene 41, past the end of the bridge). The limit is widened like the view.
// The sprite buffer is sized for every sprite in the scene up to the game's own cap, whatever the
// distance, so more of them in range cannot overflow it.

// At 0x1517DF04 in func_1517DE5C, with the distance limit in $a3 just before func_1517BBAC.
extern "C" void conker_scenery_sprite_distance(uint8_t* /*rdram*/, recomp_context* ctx) {
    const float widening = conker::display::rendered_aspect() / kPictureAspect;
    if (widening <= 1.0f || !culling_enabled()) {
        return;
    }
    ctx->r7 = static_cast<int32_t>(static_cast<float>(static_cast<int32_t>(ctx->r7)) * widening * kMargin);
}

conker::switches::Switch conker::switches::widescreen_culling() {
    return {"WIDESCREEN CULLING", culling_enabled, [](bool on) { g_culling.store(on); }};
}
