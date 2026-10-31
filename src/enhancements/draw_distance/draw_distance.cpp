// Everything seen from farther. Native helpers called from the [[patches.hook]] snippets
// in config/us.toml (func_1502E4C4, func_151787AC, func_15112A80, func_1517DE5C, func_1517BBAC).
// CONKER_DRAW_DISTANCE=0 keeps the game's distances.
//
// A scene is drawn out to its far distance (D_800D9B1C, 10000 to 20000 units from the camera), where
// its fog has closed in, but what stands in it has shorter limits of its own: characters fade out
// past 3000, props past 4500 or 5000, swarms of bees past 2500, and the scenery sprites (grass,
// flowers, mushrooms) past 1500 from Conker. Each looked fine in the 4:3 view at the game's
// resolution; widened and sharper, they visibly popped in on the way towards them, or stood missing
// in plain view (scene 12's sunflower; scene 57's grass past the dark clearing). Each limit is
// raised to kReach times the game's own, never past the scene's far distance: far enough that
// nothing pops in near the middle of the picture, while what is left out is small and deep in the
// fog, and not worth the display list it would take. Only drawing changes: what the characters
// and props do does not depend on it.
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "recomp.h"
#include "enhancements/display_options/widescreen_culling.hpp"
#include "enhancements/status.hpp"
#include "enhancements/switches.hpp"
#include "runtime/snapshot.hpp"

namespace {

constexpr uint32_t kSceneFar = 0x800D9B1C;      // float, the scene's far distance (fog, far plane)
constexpr uint32_t kSpriteCount = 0x800DDD20;   // s32, the scene's scenery sprites
constexpr uint32_t kSpriteCap = 0x800DDD0C;     // s32, the most the game builds in a frame (110)
constexpr float kReach = 2.5f;                  // how many times farther than the game's own limits

inline gpr kseg0(uint32_t address) { return static_cast<gpr>(static_cast<int32_t>(address)); }

float read_float(uint8_t* rdram, uint32_t address) {
    const uint32_t bits = MEM_W(0, kseg0(address));
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// A limit of the game's raised by kReach, but not past the scene's far distance, nor below the
// game's own (a scene drawn no farther than it, or no sensible far distance between scenes).
float reach(uint8_t* rdram, float distance) {
    const float far_distance = read_float(rdram, kSceneFar);
    if (!(far_distance > distance && far_distance < 1.0e6f)) {
        return distance;
    }
    return std::fmin(distance * kReach, far_distance);
}

std::atomic<bool> g_enabled{[] {
    const char* value = std::getenv("CONKER_DRAW_DISTANCE");
    return value == nullptr || value[0] != '0';
}()};  // the debug menu switches it

bool draw_distance_enabled() { return g_enabled.load(std::memory_order_relaxed); }

// The scenery sprites' limit this frame as the game (and the widened view) would have it, while
// the drawing reaches farther; 0 when it does not.
int32_t g_sprite_game_limit = 0;
// Native state the game's memory depends on, carried by a snapshot (runtime/snapshot.hpp).
const bool g_snapshot_fields = (conker::snapshot::add("draw_distance.sprite_game_limit", &g_sprite_game_limit, sizeof(g_sprite_game_limit)), true);

}  // namespace

// Characters: func_1502E4C4 decides, for each view, whether an actor is drawn. Past its own
// distance from the camera, u16 actor +0x10 (3000 for most, up to 7000), the actor is faded out
// (func_1506160C code 8, its per-view opacity at +0xB going to 0) and left out once the opacity is
// 0; nearer, it fades back in (code 9). A distance of 0 means always drawn. Only the opacity
// changes: the actor's script and animation run either way.
//
// Called at 0x1502E770 with the distance in $s0, where the game has already put 100000 for a
// camera that shows everything: a nonzero distance is raised. Drawing more actors cannot overflow the display list, since func_1502C974 drops an actor that
// would take the list past its limit.
extern "C" void conker_character_distance(uint8_t* rdram, recomp_context* ctx) {
    const int32_t distance = static_cast<int32_t>(ctx->r16);
    if (draw_distance_enabled() && distance > 0) {
        ctx->r16 = static_cast<int32_t>(reach(rdram, static_cast<float>(distance)));
    }
}

// Swarms of bees (see kSwarms in pause_save/resume_point.cpp) are drawn with their bees only while
// func_151787AC finds them in a view, within D_800A71E4 (2500) of the camera, so a swarm settled
// around scene 12's sunflower vanished from across the clearing. Called at 0x15178934 with that
// distance just loaded into $f22, which is raised.
extern "C" void conker_swarm_distance(uint8_t* rdram, recomp_context* ctx) {
    if (draw_distance_enabled()) {
        ctx->f22.fl = reach(rdram, ctx->f22.fl);
    }
}

// Props (the bank 0B objects): func_15112A80 draws each within its own distance of the camera, u16
// object +0x90 (0 for the scene's far distance, 1 for 40000, otherwise the distance itself; 4500 or
// 5000 for most of scene 12's), fading it over the last 200 units, and passes the distance on to the
// sphere-in-view test func_150A6360. Called at 0x15112CF0, where every way through has the distance
// in $s2: a distance other than 0 and 1 is raised. func_151137D4, which draws each prop, drops one that would take the display list past its limit,
// as func_1502C974 does for the characters.
extern "C" void conker_prop_distance(uint8_t* rdram, recomp_context* ctx) {
    const int32_t distance = static_cast<int32_t>(ctx->r18);
    if (draw_distance_enabled() && distance > 1) {
        ctx->r18 = static_cast<int32_t>(reach(rdram, static_cast<float>(distance)));
    }
}

// The scenery sprites: func_1517DE5C has func_1517BBAC build a camera-facing quad for each sprite
// (sunflowers, grass, mushrooms) within a distance of Conker, 1500, with its vertex alpha fading
// over the last 510 (min(255, (limit - distance) / 2)). The game walks its sprites, sorted along x
// or z, from behind Conker towards where the camera looks, and stops once it has built D_800DDD0C
// (110) of them; the frame's buffer is sized for that many, or for all of the scene's if fewer, so
// building more of them within range cannot overflow it. The limit is widened with the view when
// the culling is (src/enhancements/display_options/widescreen_culling.cpp), and raised as the others
// are when every sprite in the scene fits within that cap, so nothing the game would have drawn can
// be crowded out. A scene with more sprites than the cap (none seen so far) keeps the widened limit,
// since farther ones would take the place of nearer ones ahead of the camera.
//
// At 0x1517DF04 in func_1517DE5C, with the distance limit in $a3 just before func_1517BBAC.
extern "C" void conker_scenery_sprite_distance(uint8_t* rdram, recomp_context* ctx) {
    const int32_t game_limit = static_cast<int32_t>(
        static_cast<float>(static_cast<int32_t>(ctx->r7)) * conker::display::culling_widening());
    ctx->r7 = game_limit;
    g_sprite_game_limit = 0;
    const int32_t count = MEM_W(0, kseg0(kSpriteCount));
    const int32_t cap = MEM_W(0, kseg0(kSpriteCap));
    if (!draw_distance_enabled() || count > cap) {
        return;
    }
    const int32_t limit = static_cast<int32_t>(reach(rdram, static_cast<float>(game_limit)));
    if (limit > game_limit) {
        ctx->r7 = limit;
        g_sprite_game_limit = game_limit;
    }
}

// Once a frame func_1517BBAC picks one of the quads it built at random, and when that sprite's
// animation starts over, func_15170500 sets off an effect at it. Those stay among the sprites the
// game itself would have built, so the effects keep to Conker's surroundings as before. At
// 0x1517CCE4, with the quad in $v0 (its first corner's position at +0, +2 and +4, as the game
// passes it on) and $t9 nonzero to skip the effect; func_1517BBAC's arguments, Conker's
// position, are still at sp+0xD8, sp+0xDC and sp+0xE0.
extern "C" void conker_scenery_sprite_effect(uint8_t* rdram, recomp_context* ctx) {
    if (g_sprite_game_limit == 0 || ctx->r25 != 0) {
        return;
    }
    const float dx = static_cast<float>(MEM_H(0, ctx->r2) - MEM_W(0xD8, ctx->r29));
    const float dy = static_cast<float>(MEM_H(2, ctx->r2) - MEM_W(0xDC, ctx->r29));
    const float dz = static_cast<float>(MEM_H(4, ctx->r2) - MEM_W(0xE0, ctx->r29));
    if (std::sqrt(dx * dx + dy * dy + dz * dz) >= static_cast<float>(g_sprite_game_limit)) {
        ctx->r25 = 1;
    }
}

nlohmann::json conker::status::draw_distance() {
    return enhancement("draw_distance", "Everything seen from farther", draw_distance_enabled(), "CONKER_DRAW_DISTANCE");
}

conker::switches::Switch conker::switches::draw_distance() {
    return {"DRAW DISTANCE", draw_distance_enabled, [](bool on) { g_enabled.store(on); }};
}
