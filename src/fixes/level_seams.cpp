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
// s16 x, y, z, flag, s16 s, t, colour).
#include <cstdint>

#include "recomp.h"

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
constexpr int kSeamCount = sizeof(kSeams) / sizeof(kSeams[0]);

struct Found {
    uint32_t address = 0;  // the vertex's address, 0 when not found
    bool searched = false;  // searched since the scene changed
};

Found g_found[kSeamCount];
int32_t g_scene = -1;

bool vertex_at(uint8_t* rdram, uint32_t address, const Position& p) {
    return MEM_H(0, kseg0(address)) == p.x && MEM_H(2, kseg0(address)) == p.y && MEM_H(4, kseg0(address)) == p.z;
}

bool seam_at(uint8_t* rdram, uint32_t address, const Seam& seam) {
    return vertex_at(rdram, address - kVertexSize, seam.before) && vertex_at(rdram, address + kVertexSize, seam.after);
}

uint32_t find_seam(uint8_t* rdram, const Seam& seam) {
    for (uint32_t address = kRdramStart + kVertexSize; address + 2 * kVertexSize <= kRdramEnd; address += 4) {
        if ((vertex_at(rdram, address, seam.from) || vertex_at(rdram, address, seam.to)) && seam_at(rdram, address, seam)) {
            return address;
        }
    }
    return 0;
}

}  // namespace

// func_1510B958 runs once a frame before the level geometry is drawn, so the level is loaded. A
// vertex already moved is checked with three reads; a scene reloaded at the same address (RESTART)
// is moved again, and one loaded elsewhere is searched for once.
extern "C" void conker_level_seams(uint8_t* rdram, recomp_context* /*ctx*/) {
    const int32_t scene = MEM_W(0, kseg0(kScene));
    if (scene != g_scene) {
        g_scene = scene;
        for (Found& found : g_found) {
            found = Found{};
        }
    }
    for (int i = 0; i < kSeamCount; i++) {
        const Seam& seam = kSeams[i];
        Found& found = g_found[i];
        if (seam.scene != scene) {
            continue;
        }
        if (found.address != 0 && !seam_at(rdram, found.address, seam)) {
            found = Found{};
        }
        if (found.address == 0) {
            if (found.searched) {
                continue;
            }
            found.searched = true;
            found.address = find_seam(rdram, seam);
            if (found.address == 0) {
                continue;
            }
        }
        if (vertex_at(rdram, found.address, seam.from)) {
            MEM_H(0, kseg0(found.address)) = seam.to.x;
            MEM_H(2, kseg0(found.address)) = seam.to.y;
            MEM_H(4, kseg0(found.address)) = seam.to.z;
        }
    }
}
