// Closes cracks in Rare's level geometry that show the sky through a wall. Native helper called from
// the [[patches.hook]] snippet for func_1510B958 in config/us.toml.
//
// A crack is a T-junction: one surface's edge runs straight from A to B while the surface beside it
// bends at a vertex M between them, and M sits slightly off the line A-B. The sliver between the
// two edges is left undrawn. On the console it is under a pixel wide, but at a higher resolution it
// shows as a dotted line of sky. Each fix moves the long edge's end vertex B, in the vertex buffer
// of that one surface only, so the edge passes just behind M and the two surfaces overlap.
//
// The level's vertices are loaded into the heap, at an address that depends on what was loaded
// before, so each fix finds its vertex by the positions of its neighbours (vertices are 16 bytes:
// s16 x, y, z, flag, s16 s, t, colour). The heap can hold more than one copy of a level's vertices
// (the Heist multiplayer map, scene 26, keeps a stale one below the drawn one), so every copy found is
// moved, and the heap is searched again every kSearchFrames frames for a copy loaded since.
#include <cstdint>
#include <cstring>

#include "recomp.h"
#include "enhancements/my_view/my_view.hpp"
#include "runtime/snapshot.hpp"

namespace {

inline gpr kseg0(uint32_t address) { return static_cast<gpr>(static_cast<int32_t>(address)); }

constexpr uint32_t kScene = 0x800BE9F0;  // D_800BE9F0: the scene being played
constexpr uint32_t kRdramStart = 0x80000400;
constexpr uint32_t kRdramEnd = 0x80800000;
constexpr int kVertexSize = 16;

struct Position {
    int16_t x, y, z;
};

struct Seam {
    int32_t scene;
    Position before;  // the vertex loaded just before the one to move
    Position from;    // the vertex to move, as Rare placed it
    Position after;   // the vertex loaded just after it
    Position to;      // where it goes
};

// Scene 48 (the Colours multiplayer map): the lookout booth's doorway onto the ramp. The ramp's
// right wall runs from the door's foot (1352, 737, -3675) to the gable's peak (1368, 1475, -3662),
// past the door post's top (1363, 1231, -3667), which is 0.76 units off that line toward the booth's
// wall. Moving the peak to (1369, 1475, -3665) puts the ramp's wall 1.4 units behind the post instead.
constexpr Seam kSeams[] = {
    {48, {1352, 737, -3675}, {1368, 1475, -3662}, {378, 993, -4224}, {1369, 1475, -3665}},
};

// Frames until the next search. A search reads one word every 8 bytes of RDRAM (vertex buffers are
// loaded by DMA, so 8-byte aligned).
constexpr int32_t kSearchFrames = 30;

int32_t g_wait = 0;
int32_t g_scene = -1;
// Native state the game's memory depends on, carried by a snapshot (runtime/snapshot.hpp).
const bool g_snapshot_fields = [] {
    conker::snapshot::add("level_seams.wait", &g_wait, sizeof(g_wait), [](const uint8_t* bytes) {
        int32_t wait;
        std::memcpy(&wait, bytes, sizeof(wait));
        return wait >= 0 && wait <= kSearchFrames;
    });
    conker::snapshot::add("level_seams.scene", &g_scene, sizeof(g_scene));
    return true;
}();

bool vertex_at(uint8_t* rdram, uint32_t address, const Position& p) {
    return MEM_H(0, kseg0(address)) == p.x && MEM_H(2, kseg0(address)) == p.y && MEM_H(4, kseg0(address)) == p.z;
}

bool seam_at(uint8_t* rdram, uint32_t address, const Seam& seam) {
    return vertex_at(rdram, address - kVertexSize, seam.before) && vertex_at(rdram, address + kVertexSize, seam.after);
}

// The first word of a position: its x and y.
uint32_t first_word(const Position& p) {
    return (static_cast<uint32_t>(static_cast<uint16_t>(p.x)) << 16) | static_cast<uint16_t>(p.y);
}

}  // namespace

// func_1510B958 runs once a frame before the level geometry is drawn, so the level is loaded.
extern "C" void conker_level_seams(uint8_t* rdram, recomp_context* /*ctx*/) {
    // The online "my view" pass draws again from memory that is put back after; the frames counted
    // here have to be the game's own.
    if (conker::my_view::drawing_again()) {
        return;
    }
    const int32_t scene = MEM_W(0, kseg0(kScene));
    if (scene != g_scene) {
        g_scene = scene;
        g_wait = 0;
    }
    bool here = false;
    for (const Seam& seam : kSeams) {
        here = here || seam.scene == scene;
    }
    if (!here) {
        return;
    }
    if (g_wait > 0) {
        g_wait--;
        return;
    }
    g_wait = kSearchFrames;
    for (const Seam& seam : kSeams) {
        if (seam.scene != scene) {
            continue;
        }
        const uint32_t first = first_word(seam.from);
        for (uint32_t address = kRdramStart + kVertexSize; address + 2 * kVertexSize <= kRdramEnd; address += 8) {
            if (static_cast<uint32_t>(MEM_W(0, kseg0(address))) == first && vertex_at(rdram, address, seam.from) &&
                seam_at(rdram, address, seam)) {
                MEM_H(0, kseg0(address)) = seam.to.x;
                MEM_H(2, kseg0(address)) = seam.to.y;
                MEM_H(4, kseg0(address)) = seam.to.z;
            }
        }
    }
}
