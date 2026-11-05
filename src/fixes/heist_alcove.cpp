// Closes a slit of background that flickers at the foot of the rocket launcher's alcove in the
// Heist multiplayer map (scene 26), seen from the red team's starting corridor. Native helper
// called from the [[patches.hook]] snippet for func_1510B958 in config/us.toml.
//
// The alcove above the corridor's far doorway is the mouth of a tunnel that runs away from the
// corridor, under a ceiling sloping down to y 659 at its far end, where it opens into a room. From
// the start of the corridor, a thin band of sight passes over the top of the dark beam below the
// alcove and under the end of that ceiling, into the room beyond, which the game does not draw from
// there, so nothing is drawn in it and it keeps whatever the frame buffer held (in a match, often a
// piece of the pink corridor wall). On the console the band is about a pixel high; at a higher
// resolution it shows as broken lines whose pieces change with every step of Conker's idle bob. Carrying the ceiling on along its own slope across the room's entrance (its two
// far vertices, with their texture coordinate along the tunnel) puts the ceiling behind the beam's top
// edge from there, so the ceiling meets the beam on screen. The ceiling ends under the entrance's own
// higher ceiling, short of the room.
//
// The level's vertices are loaded into the heap, at an address that depends on what was loaded
// before, so the ceiling is found by its vertices' positions (vertices are 16 bytes: s16 x, y, z,
// flag, s16 s, t, colour). The heap can hold more than one copy of the level's vertices, of which only
// the later one is drawn, so every copy found is moved, and the heap is searched again every
// kSearchFrames frames for a copy loaded since.
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "recomp.h"
#include "enhancements/my_view/my_view.hpp"
#include "runtime/snapshot.hpp"

namespace {

inline gpr kseg0(uint32_t address) { return static_cast<gpr>(static_cast<int32_t>(address)); }

constexpr uint32_t kScene = 0x800BE9F0;  // D_800BE9F0: the scene being played
constexpr int32_t kHeistScene = 26;
constexpr uint32_t kRdramStart = 0x80000400;
constexpr uint32_t kRdramEnd = 0x80800000;
constexpr int kVertexSize = 16;
constexpr int kCount = 4;  // the vertices checked: the ceiling's two far vertices between their partners

struct Vertex {
    int16_t x, y, z, s;
};

// The tunnel ceiling's far section as loaded: a near corner, the two far corners (at the top of
// the room's entrance), the other near corner.
constexpr Vertex kCeiling[kCount] = {
    {2140, 844, -503, 9232},
    {1061, 659, 152, 8208},
    {1071, 659, -409, 8208},
    {2152, 844, 86, 9232},
};
// The far corners carried on along the ceiling's slope by about 200 units (0.19 of the ceiling's
// length, its texture coordinate with it), to just short of the room: from the corridor's start that
// closes the band for a camera up to about 75 units above Conker's standing one. The tunnel narrows
// toward its end, so the -z corner is widened (to -420, inside the entrance's wall) rather than
// carried on inward, which would leave a sliver beside that wall.
constexpr int kFar[2] = {1, 2};
constexpr Vertex kMoved[2] = {
    {854, 624, 165, 8013},
    {868, 624, -420, 8013},
};

// Frames until the next search. The search reads one word every 8 bytes of RDRAM.
constexpr int32_t kSearchFrames = 30;

int32_t g_wait = 0;
int32_t g_scene = -1;
// Native state the game's memory depends on, carried by a snapshot (runtime/snapshot.hpp).
const bool g_snapshot_fields = [] {
    conker::snapshot::add("heist_alcove.wait", &g_wait, sizeof(g_wait), [](const uint8_t* bytes) {
        int32_t wait;
        std::memcpy(&wait, bytes, sizeof(wait));
        return wait >= 0 && wait <= kSearchFrames;
    });
    conker::snapshot::add("heist_alcove.scene", &g_scene, sizeof(g_scene));
    return true;
}();

bool position_at(uint8_t* rdram, uint32_t address, const Vertex& v) {
    return MEM_H(0, kseg0(address)) == v.x && MEM_H(2, kseg0(address)) == v.y && MEM_H(4, kseg0(address)) == v.z;
}

// The ceiling as Rare placed it, not yet moved.
bool ceiling_at(uint8_t* rdram, uint32_t address) {
    for (int i = 0; i < kCount; i++) {
        if (!position_at(rdram, address + i * kVertexSize, kCeiling[i])) {
            return false;
        }
    }
    return true;
}

void move_ceiling(uint8_t* rdram, uint32_t address) {
    for (int k = 0; k < 2; k++) {
        const uint32_t vertex = address + kFar[k] * kVertexSize;
        MEM_H(0, kseg0(vertex)) = kMoved[k].x;
        MEM_H(2, kseg0(vertex)) = kMoved[k].y;
        MEM_H(4, kseg0(vertex)) = kMoved[k].z;
        MEM_H(8, kseg0(vertex)) = kMoved[k].s;
    }
}

}  // namespace

// func_1510B958 runs once a frame before the level geometry is drawn, so the level is loaded.
extern "C" void conker_heist_alcove(uint8_t* rdram, recomp_context* /*ctx*/) {
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
    if (scene != kHeistScene) {
        return;
    }
    if (g_wait > 0) {
        g_wait--;
        return;
    }
    g_wait = kSearchFrames;
    // The first vertex's x and y as one word, so most of the search is one read.
    const uint32_t first = (static_cast<uint32_t>(static_cast<uint16_t>(kCeiling[0].x)) << 16) |
                           static_cast<uint16_t>(kCeiling[0].y);
    for (uint32_t address = kRdramStart; address + kCount * kVertexSize <= kRdramEnd; address += 8) {
        if (static_cast<uint32_t>(MEM_W(0, kseg0(address))) == first && ceiling_at(rdram, address)) {
            move_ceiling(rdram, address);
        }
    }
}
