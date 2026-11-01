// The Heist alien casts a projected shadow like Conker's. Native helpers called from the [[patches.hook]]
// snippets in config/us.toml (func_15186794, func_15184DF0, func_1502CCFC).
//
// func_15186794 casts a shadow for each actor whose +0x66 has 0x10 and whose kind has shadow lists in
// D_800C48F0 (a model's lists less their texture loads, from its header's +0x28). It draws them into a
// 64x64 8-bit silhouette from a light (func_15184368), then lays the silhouette on the collision near the
// actor (func_1518652C, func_151873E4) and fades it out with depth below the caster. The alien (kind 94)
// has neither the flag nor shadow lists, and in its cutscenes the rest of that falls short too:
// - its own light is a torch that throws the shadow sideways, so it is lit from straight above instead;
// - the cutscenes film it zoomed in from up to 6000 units away, past the 2000 the game casts shadows to;
// - they animate its body hundreds of units away from its position (+0x14), which the shadow is built
//   around, so for the shadow it stands where its bones are;
// - the lab's collision by the door is only studs, and the corridor it walks in from has none, so its
//   shadow falls on a flat quad under it at its floor's height (beyond the collision, its feet's).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "recomp.h"

namespace {

inline gpr kseg0(uint32_t address) { return static_cast<gpr>(static_cast<int32_t>(address)); }

float load_float(uint8_t* rdram, gpr address) {
    const int32_t word = MEM_W(0, address);
    float value;
    std::memcpy(&value, &word, 4);
    return value;
}

void store_float(uint8_t* rdram, gpr address, float value) {
    int32_t word;
    std::memcpy(&word, &value, 4);
    MEM_W(0, address) = word;
}

constexpr uint32_t kActors = 0x800CC2D0;         // D_800CC2D0: the actors, 0x32C bytes each
constexpr uint32_t kActorSize = 0x32C;
constexpr int kActorCount = 25;
constexpr uint32_t kModelLists = 0x800C4488;     // D_800C4488[kind]: a model's display list per bone
constexpr uint32_t kShadowLists = 0x800C48F0;    // D_800C48F0[kind]: its shadow lists, or 0
constexpr uint32_t kBoneCounts = 0x800C4778;     // D_800C4778[kind]: u16, how many
constexpr uint32_t kOverheadOnly = 0x800DE034;   // D_800DE034: nonzero to skip the point lights
constexpr uint32_t kOverheadLight = 0x800DE010;  // D_800DE010: the overhead light, a float vector from the caster
constexpr uint32_t kFadeNear = 0x800D3688;       // D_800D3688/D_800D368C: the light-space depths a shadow
constexpr uint32_t kFadeFar = 0x800D368C;        // fades out between (full at the far one, by the caster)
constexpr uint8_t kAlienKind = 94;
constexpr uint8_t kCastsShadow = 0x10;           // actor +0x66
// Free RDRAM above the game's 4 MiB (see pause_save.cpp): the alien's shadow lists, then its floor quad.
constexpr uint32_t kListArea = 0x807E0000;
constexpr uint32_t kListAreaEnd = 0x807EE000;
constexpr uint32_t kFloorVtx = 0x807EE000;       // 4 Vtx

// What conker_alien_shadow_begin changed, put back by conker_alien_shadow_end.
struct Pass {
    bool active = false;
    int saved_flags[kActorCount];  // the alien's +0x66, or -1 for every other actor
    int saved_drawn[kActorCount];  // +0x123, which drawing the silhouette sets
    int32_t saved_x[kActorCount];
    int32_t saved_z[kActorCount];
    bool moved[kActorCount];
    bool lent_lists = false;
    bool light_changed = false;
    int32_t saved_overhead_only = 0;
    int32_t saved_overhead[3] = {};
    bool alien = false;  // the actor func_15186794 is casting for is the alien
} g_pass;

uint32_t g_built_for = 0;         // the model lists the shadow lists were built from
float g_body[2] = {};             // x, z of the alien's bones' centre, at its last draw
bool g_body_known = false;
float g_floor = 0.0f;             // the height of the last flat collision under it
bool g_floor_known = false;
float g_feet[16];                 // its feet's height at each of its last 16 draws
int g_feet_next = 0;
int g_feet_count = 0;
float g_beyond = 0.0f;            // the floor beyond the collision, eased toward its feet's height
bool g_beyond_known = false;

// Copies one display list without its texture loads and its render mode, as Rare's shadow lists are
// (Conker's: his model's commands less the loads). The alien's model sets its own render mode (G_SETOTHERMODE,
// depth compare on), which in place of the silhouette's left a column of the next model undrawn under RT64.
// Returns the next free address, or 0 when out of room.
uint32_t copy_without_loads(uint8_t* rdram, uint32_t from, uint32_t to) {
    for (int i = 0; i < 4096; i++, from += 8) {
        if (to + 8 > kListAreaEnd || (from & 0xFF800000) != 0x80000000) {
            return 0;
        }
        const uint32_t w0 = static_cast<uint32_t>(MEM_W(0, kseg0(from)));
        const uint32_t w1 = static_cast<uint32_t>(MEM_W(4, kseg0(from)));
        const uint8_t op = w0 >> 24;
        const bool dropped = op == 0xFD || op == 0xF3 || op == 0xF4 || op == 0xF0 || op == 0xE6 || op == 0xE8 ||
                             op == 0xEF || (op == 0xF5 && ((w1 >> 24) & 7) == 7);  // G_TX_LOADTILE
        if (!dropped) {
            MEM_W(0, kseg0(to)) = static_cast<int32_t>(w0);
            MEM_W(4, kseg0(to)) = static_cast<int32_t>(w1);
            to += 8;
        }
        if (op == 0xDF || (op == 0xDE && (w0 & 0x00FF0000) != 0)) {  // the end, or a branch that does not return
            return to;
        }
    }
    return 0;
}

// The alien's shadow lists at kListArea: a pointer per bone, then the lists. Built again when its model loads
// anew, with what was learnt about the last scene's floor forgotten.
uint32_t shadow_lists(uint8_t* rdram) {
    const uint32_t model = static_cast<uint32_t>(MEM_W(0, kseg0(kModelLists + kAlienKind * 4)));
    if (model == 0) {
        return 0;
    }
    if (model == g_built_for) {
        return kListArea;
    }
    g_built_for = 0;
    g_body_known = g_floor_known = g_beyond_known = false;
    g_feet_count = 0;
    const int bones = MEM_HU(kAlienKind * 2, kseg0(kBoneCounts));
    if (bones <= 0 || bones > 128) {
        return 0;
    }
    uint32_t to = kListArea + bones * 4;
    for (int b = 0; b < bones; b++) {
        const uint32_t list = static_cast<uint32_t>(MEM_W(b * 4, kseg0(model)));
        MEM_W(b * 4, kseg0(kListArea)) = static_cast<int32_t>(to);
        to = copy_without_loads(rdram, list, to);
        if (to == 0) {
            return 0;
        }
    }
    g_built_for = model;
    return kListArea;
}

bool alien(uint8_t* rdram, int32_t index) {
    return index >= 0 && index < kActorCount && MEM_BU(4, kseg0(kActors + index * kActorSize)) == kAlienKind;
}

}  // namespace

// At func_15186794's entry: the alien casts a shadow from its shadow lists, standing where its body is.
extern "C" void conker_alien_shadow_begin(uint8_t* rdram) {
    if (g_pass.active) {
        return;  // active: the object inspector runs the function again from inside its own hook
    }
    bool any = false;
    for (int i = 0; i < kActorCount; i++) {
        const gpr actor = kseg0(kActors + i * kActorSize);
        g_pass.saved_flags[i] = -1;
        if (MEM_W(0, actor) == 0 || MEM_BU(4, actor) != kAlienKind) {
            continue;
        }
        g_pass.saved_flags[i] = MEM_BU(0x66, actor);
        g_pass.saved_drawn[i] = MEM_BU(0x123, actor);
        MEM_B(0x66, actor) = static_cast<int8_t>(MEM_BU(0x66, actor) | kCastsShadow);
        g_pass.moved[i] = g_body_known;
        if (g_body_known) {
            g_pass.saved_x[i] = MEM_W(0x14, actor);
            g_pass.saved_z[i] = MEM_W(0x1C, actor);
            store_float(rdram, actor + 0x14, g_body[0]);
            store_float(rdram, actor + 0x1C, g_body[1]);
        }
        any = true;
    }
    if (!any) {
        return;
    }
    g_pass.active = true;
    const gpr shadow = kseg0(kShadowLists + kAlienKind * 4);
    const uint32_t lists = shadow_lists(rdram);
    g_pass.lent_lists = MEM_W(0, shadow) == 0 && lists != 0;
    if (g_pass.lent_lists) {
        MEM_W(0, shadow) = static_cast<int32_t>(lists);
    }
}

// Before func_15186794 asks func_15184368 for the light of actor $sp+0xF8: the alien's comes from straight
// above it (a little to one side, or the light falls inside its shadow box), high enough to clear it.
extern "C" void conker_alien_shadow_light(uint8_t* rdram, recomp_context* ctx) {
    g_pass.alien = g_pass.active && alien(rdram, MEM_W(0xF8, ctx->r29));
    if (!g_pass.alien) {
        return;
    }
    g_pass.saved_overhead_only = MEM_W(0, kseg0(kOverheadOnly));
    for (int i = 0; i < 3; i++) {
        g_pass.saved_overhead[i] = MEM_W(i * 4, kseg0(kOverheadLight));
    }
    g_pass.light_changed = true;
    MEM_W(0, kseg0(kOverheadOnly)) = 1;
    const float light[3] = {1.0f, 1200.0f, -300.0f};
    for (int i = 0; i < 3; i++) {
        store_float(rdram, kseg0(kOverheadLight + i * 4), light[i]);
    }
}

// After func_15184368: the scene's own light back, and the alien's fade lengthened. Lit from 1200 units up,
// its floor is most of the way along the game's fade, where its shadow was all but gone.
extern "C" void conker_alien_shadow_light_done(uint8_t* rdram) {
    if (!g_pass.light_changed) {
        return;
    }
    g_pass.light_changed = false;
    for (int i = 0; i < 3; i++) {
        MEM_W(i * 4, kseg0(kOverheadLight)) = g_pass.saved_overhead[i];
    }
    MEM_W(0, kseg0(kOverheadOnly)) = g_pass.saved_overhead_only;
    store_float(rdram, kseg0(kFadeNear), load_float(rdram, kseg0(kFadeFar)) - 4000.0f);
}

// After func_151873E4 added the props to the receivers func_1518652C found: $v0 vertex records at $s5, 20 bytes
// each and three to a triangle (the vertex in the light's space as floats, by the matrix at $s1, then the address
// of a Vtx holding it in the world). The alien's are replaced by a flat quad under its body: by the lab's door the
// receivers are only studs and a few floor triangles, and beyond it there are none, so the shadow came and went.
// Its height is the flat triangle under the body's, else the last one's; beyond the collision (no receivers at
// all) its feet's, eased, and lifted a little so the corridor's drawn floor does not hide it.
extern "C" void conker_alien_shadow_floor(uint8_t* rdram, recomp_context* ctx) {
    if (!g_pass.alien) {
        return;
    }
    const int32_t count = static_cast<int32_t>(ctx->r2);
    const gpr records = ctx->r21;
    for (int t = 0; t + 2 < count; t += 3) {
        float x[3], y[3], z[3];
        for (int v = 0; v < 3; v++) {
            const gpr vtx = static_cast<gpr>(MEM_W((t + v) * 20 + 12, records));
            x[v] = static_cast<float>(MEM_H(0, vtx));
            y[v] = static_cast<float>(MEM_H(2, vtx));
            z[v] = static_cast<float>(MEM_H(4, vtx));
        }
        if (std::fmax(std::fmax(y[0], y[1]), y[2]) - std::fmin(std::fmin(y[0], y[1]), y[2]) > 8.0f) {
            continue;  // not flat
        }
        float side[3];
        for (int e = 0; e < 3; e++) {
            const int n = (e + 1) % 3;
            side[e] = (x[n] - x[e]) * (g_body[1] - z[e]) - (z[n] - z[e]) * (g_body[0] - x[e]);
        }
        if ((side[0] >= 0 && side[1] >= 0 && side[2] >= 0) || (side[0] <= 0 && side[1] <= 0 && side[2] <= 0)) {
            g_floor = (y[0] + y[1] + y[2]) / 3.0f;
            g_floor_known = true;
            break;
        }
    }
    if (!g_floor_known || !g_body_known) {
        return;
    }
    float floor = g_floor + 1.0f;
    if (count > 0) {
        g_beyond_known = false;  // over the lab, where it also hangs from the ceiling: the lab's floor
    } else if (g_feet_count >= 4) {
        float feet[16];
        std::copy(g_feet, g_feet + g_feet_count, feet);
        std::sort(feet, feet + g_feet_count);
        const float target = std::clamp(feet[g_feet_count / 2], g_floor, g_floor + 150.0f);
        g_beyond = g_beyond_known ? g_beyond + std::clamp(target - g_beyond, -3.0f, 3.0f) : target;
        g_beyond_known = true;
        floor = g_beyond + 8.0f;
    }
    constexpr float kHalf = 400.0f;
    const float corner[4][2] = {{-kHalf, -kHalf}, {kHalf, -kHalf}, {kHalf, kHalf}, {-kHalf, kHalf}};
    for (int c = 0; c < 4; c++) {
        const gpr vtx = kseg0(kFloorVtx + c * 16);
        MEM_H(0, vtx) = static_cast<int16_t>(g_body[0] + corner[c][0]);
        MEM_H(2, vtx) = static_cast<int16_t>(floor);
        MEM_H(4, vtx) = static_cast<int16_t>(g_body[1] + corner[c][1]);
        MEM_H(6, vtx) = 0;
        MEM_W(8, vtx) = 0;
        MEM_W(12, vtx) = static_cast<int32_t>(0xFFFFFFFF);
    }
    // Both windings: the projection keeps only the triangles facing the light.
    const gpr light = ctx->r17;
    const int order[12] = {0, 1, 2, 0, 2, 3, 0, 2, 1, 0, 3, 2};
    for (int i = 0; i < 12; i++) {
        const gpr vtx = kseg0(kFloorVtx + order[i] * 16);
        const float p[3] = {static_cast<float>(MEM_H(0, vtx)), static_cast<float>(MEM_H(2, vtx)),
                            static_cast<float>(MEM_H(4, vtx))};
        for (int j = 0; j < 3; j++) {
            float v = load_float(rdram, light + 48 + j * 4);  // row vectors: the translation last
            for (int k = 0; k < 3; k++) {
                v += p[k] * load_float(rdram, light + (k * 4 + j) * 4);
            }
            store_float(rdram, records + i * 20 + j * 4, v);
        }
        MEM_W(i * 20 + 12, records) = static_cast<int32_t>(kFloorVtx + order[i] * 16);
        MEM_W(i * 20 + 16, records) = 0;
    }
    ctx->r2 = 12;
}

// Before func_15186794 returns: everything conker_alien_shadow_begin changed, put back.
extern "C" void conker_alien_shadow_end(uint8_t* rdram) {
    if (!g_pass.active) {
        return;
    }
    g_pass.active = false;
    g_pass.alien = false;
    for (int i = 0; i < kActorCount; i++) {
        if (g_pass.saved_flags[i] < 0) {
            continue;
        }
        const gpr actor = kseg0(kActors + i * kActorSize);
        if (g_pass.moved[i]) {
            MEM_W(0x14, actor) = g_pass.saved_x[i];
            MEM_W(0x1C, actor) = g_pass.saved_z[i];
        }
        MEM_B(0x66, actor) = static_cast<int8_t>(g_pass.saved_flags[i]);
        MEM_B(0x123, actor) = static_cast<int8_t>(g_pass.saved_drawn[i]);
    }
    if (g_pass.lent_lists) {
        MEM_W(0, kseg0(kShadowLists + kAlienKind * 4)) = 0;
        g_pass.lent_lists = false;
    }
}

// In func_15184DF0, $v0 is the camera's distance from actor $sp+0x28, about to be tested against 2000. The
// cutscenes film the alien zoomed in from further than that: out to 8000 its distance counts as 2000.
extern "C" void conker_alien_shadow_distance(uint8_t* rdram, recomp_context* ctx) {
    if (!g_pass.active || !alien(rdram, MEM_W(0x28, ctx->r29))) {
        return;
    }
    const int32_t distance = static_cast<int32_t>(ctx->r2);
    if (distance > 2000 && distance <= 8000) {
        ctx->r2 = 2000;
    }
}

// At func_1502CCFC(dl, actor, ?, matrices, mode at $sp+0x10), which draws an actor's model (mode 255: as itself).
// The matrices are its bones' as floats, their world positions in the last row: their centre is the alien's body.
extern "C" void conker_alien_shadow_bones(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t matrices = static_cast<uint32_t>(ctx->r7);
    if (!alien(rdram, static_cast<int32_t>(ctx->r5)) || MEM_W(0x10, ctx->r29) != 255 ||
        (matrices & 0xFF800000) != 0x80000000) {
        return;
    }
    const int bones = MEM_HU(kAlienKind * 2, kseg0(kBoneCounts));
    if (bones <= 0 || bones > 128) {
        return;
    }
    float sum[2] = {};
    for (int b = 0; b < bones; b++) {
        sum[0] += load_float(rdram, kseg0(matrices + b * 0x40 + 48));
        sum[1] += load_float(rdram, kseg0(matrices + b * 0x40 + 56));
    }
    g_body[0] = sum[0] / bones;
    g_body[1] = sum[1] / bones;
    g_body_known = true;
}

// Before func_1502CCFC stores segment 1 ($t8, the model's vertices) for actor $s2, with $s3 its bone matrices
// and $sp+0x160 the mode. The shadow lists are the model's lists, so each G_VTX there follows the G_MTX of the
// bone it hangs from: its vertices' heights, low end first, are its feet's. The 5th percentile, as now and then
// a stray vertex or a bone not yet placed dips far below.
extern "C" void conker_alien_shadow_mesh(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t vertices = static_cast<uint32_t>(ctx->r24);
    const uint32_t matrices = static_cast<uint32_t>(ctx->r19);
    if (g_built_for == 0 || !alien(rdram, static_cast<int32_t>(ctx->r18)) ||
        MEM_W(0x160, ctx->r29) != 255 || (vertices & 0xFF800000) != 0x80000000 ||
        (matrices & 0xFF800000) != 0x80000000) {
        return;
    }
    static float heights[4096];
    int count = 0;
    const int bones = MEM_HU(kAlienKind * 2, kseg0(kBoneCounts));
    for (int b = 0; b < bones; b++) {
        uint32_t mtx = 0;
        for (uint32_t a = static_cast<uint32_t>(MEM_W(b * 4, kseg0(kListArea))); a < kListAreaEnd; a += 8) {
            const uint32_t w0 = static_cast<uint32_t>(MEM_W(0, kseg0(a)));
            const uint32_t w1 = static_cast<uint32_t>(MEM_W(4, kseg0(a)));
            const uint8_t op = w0 >> 24;
            if (op == 0xDF) {
                break;
            }
            if (op == 0xDA && (w1 >> 24) == 0x03) {
                mtx = matrices + (w1 & 0xFFFFFF);
            } else if (op == 0x01 && (w1 >> 24) == 0x01 && mtx != 0) {
                const int n = (w0 >> 12) & 0xFF;
                float column[4];
                for (int k = 0; k < 4; k++) {
                    column[k] = load_float(rdram, kseg0(mtx + (k * 4 + 1) * 4));
                }
                for (int v = 0; v < n && count < 4096; v++) {
                    const gpr vtx = kseg0(vertices + (w1 & 0xFFFFFF) + v * 16);
                    heights[count++] = MEM_H(0, vtx) * column[0] + MEM_H(2, vtx) * column[1] +
                                       MEM_H(4, vtx) * column[2] + column[3];
                }
            }
        }
    }
    if (count <= 20) {
        return;
    }
    std::nth_element(heights, heights + count / 20, heights + count);
    g_feet[g_feet_next] = heights[count / 20];
    g_feet_next = (g_feet_next + 1) % 16;
    g_feet_count = std::min(g_feet_count + 1, 16);
}
