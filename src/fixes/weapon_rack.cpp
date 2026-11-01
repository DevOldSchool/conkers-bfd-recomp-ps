// Stops the weapon rack in the Heist's white room (scene 64) from z-fighting where its bars cross. Native
// helper called from the [[patches.hook]] snippet for func_1510B958 in config/us.toml.
//
// The rack's rails and posts are three-sided prisms, and on each of its two frames every rail and post
// has its flat side in the same plane (model z -2 on one frame, -40 on the other). Seen from that side
// (the "OH! HO!" shot looks through the rack at Conker), the flat sides overlap exactly where a post
// crosses a rail. The console stores Z in 18 bits, compressed, so the two faces come out equal there and
// the one drawn first, the rail, stays in front. RT64 keeps 32-bit float depth, so each triangle's own
// rounding picks the winner and the crossing shows a sawtooth that crawls as the camera moves. Moving each
// rail's flat side one unit out (its four corner vertices, shared with its slanted sides) keeps the rail
// in front everywhere, as on the console. Seen from the other side the flat sides are culled.
//
// The rack's vertices are loaded into the heap with its model, at an address that depends on what was
// loaded before, so they are found by their positions (vertices are 16 bytes: s16 x, y, z, flag, s16 s,
// t, colour). Every rack section in the room draws the same vertices.
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "recomp.h"
#include "runtime/snapshot.hpp"

namespace {

inline gpr kseg0(uint32_t address) { return static_cast<gpr>(static_cast<int32_t>(address)); }

constexpr uint32_t kScene = 0x800BE9F0;  // D_800BE9F0: the scene being played
constexpr int32_t kHeistScene = 64;
constexpr uint32_t kRdramStart = 0x80000400;
constexpr uint32_t kRdramEnd = 0x80800000;
constexpr int kVertexSize = 16;

struct Position {
    int16_t x, y, z;
};

// Each rail is six vertices: the two ends of its ridge, then the four corners of its flat side. Offsets
// are from the first rail's first vertex.
constexpr uint32_t kRails[] = {0x000, 0x060, 0x0C0, 0x120, 0x5A0};
constexpr int kRailCount = sizeof(kRails) / sizeof(kRails[0]);
constexpr int kCorners = 4;
constexpr uint32_t kCornerOffsets[kCorners] = {0x10, 0x30, 0x40, 0x50};
constexpr int16_t kRailLeft = -533;
constexpr int16_t kRailRight = 495;
constexpr int16_t kFlatFront = -2;  // the frame nearer the room, ridges at z 5
constexpr int16_t kFlatBack = -40;  // the frame against the wall, ridges at z -33
constexpr int16_t kMove = -1;       // toward the flat side

// The first rail's first vertex, the end of the back frame's middle rail's ridge, and the one after it.
constexpr Position kFirst = {kRailRight, 101, -33};
constexpr Position kSecond = {kRailRight, 105, kFlatBack};

// The white room is loaded partway through the scene, so until the rack is found it is searched for
// every kSearchFrames frames.
constexpr int32_t kSearchFrames = 30;

uint32_t g_address = 0;  // the first rail's first vertex, 0 when not found
int32_t g_wait = 0;      // frames until the next search
int32_t g_scene = -1;
// Native state the game's memory depends on, carried by a snapshot (runtime/snapshot.hpp).
const bool g_snapshot_fields = [] {
    // The address is read (and written) through: one inside RDRAM where find_rack can find one, or 0.
    conker::snapshot::add("weapon_rack.address", &g_address, sizeof(g_address), [](const uint8_t* bytes) {
        uint32_t address;
        std::memcpy(&address, bytes, sizeof(address));
        return address == 0 ||
               (address % 4 == 0 && address >= kRdramStart && address + kRails[kRailCount - 1] + 6 * kVertexSize <= kRdramEnd);
    });
    conker::snapshot::add("weapon_rack.wait", &g_wait, sizeof(g_wait), [](const uint8_t* bytes) {
        int32_t wait;
        std::memcpy(&wait, bytes, sizeof(wait));
        return wait >= 0 && wait <= kSearchFrames;
    });
    conker::snapshot::add("weapon_rack.scene", &g_scene, sizeof(g_scene));
    return true;
}();

bool vertex_at(uint8_t* rdram, uint32_t address, const Position& p) {
    return MEM_H(0, kseg0(address)) == p.x && MEM_H(2, kseg0(address)) == p.y && MEM_H(4, kseg0(address)) == p.z;
}

// A corner as Rare placed it or as moved: at the rail's left or right end, on either frame's flat side.
bool corner_at(uint8_t* rdram, uint32_t address) {
    const int16_t x = MEM_H(0, kseg0(address));
    const int16_t z = MEM_H(4, kseg0(address));
    return (x == kRailLeft || x == kRailRight) &&
           (z == kFlatFront || z == kFlatBack || z == kFlatFront + kMove || z == kFlatBack + kMove);
}

bool rack_at(uint8_t* rdram, uint32_t address) {
    if (!vertex_at(rdram, address, kFirst)) {
        return false;
    }
    for (uint32_t rail : kRails) {
        for (uint32_t corner : kCornerOffsets) {
            if (!corner_at(rdram, address + rail + corner)) {
                return false;
            }
        }
    }
    return true;
}

uint32_t find_rack(uint8_t* rdram) {
    for (uint32_t address = kRdramStart; address + kRails[kRailCount - 1] + 6 * kVertexSize <= kRdramEnd; address += 4) {
        if (vertex_at(rdram, address, kFirst) && (vertex_at(rdram, address + kVertexSize, kSecond) ||
                                                  vertex_at(rdram, address + kVertexSize, {kRailRight, 105, kFlatBack + kMove})) &&
            rack_at(rdram, address)) {
            return address;
        }
    }
    return 0;
}

}  // namespace

// func_1510B958 runs once a frame before the level geometry is drawn. Moved corners are left alone, so a
// rack already fixed is checked with a few reads each frame.
extern "C" void conker_weapon_rack(uint8_t* rdram, recomp_context* /*ctx*/) {
    const int32_t scene = MEM_W(0, kseg0(kScene));
    if (scene != g_scene) {
        g_scene = scene;
        g_address = 0;
        g_wait = 0;
    }
    if (scene != kHeistScene) {
        return;
    }
    if (g_address != 0 && !rack_at(rdram, g_address)) {
        g_address = 0;
    }
    if (g_address == 0) {
        if (g_wait > 0) {
            g_wait--;
            return;
        }
        g_wait = kSearchFrames;
        g_address = find_rack(rdram);
        if (g_address == 0) {
            return;
        }
    }
    for (uint32_t rail : kRails) {
        for (uint32_t corner : kCornerOffsets) {
            const uint32_t address = g_address + rail + corner;
            const int16_t z = MEM_H(4, kseg0(address));
            if (z == kFlatFront || z == kFlatBack) {
                MEM_H(4, kseg0(address)) = static_cast<int16_t>(z + kMove);
            }
        }
    }
}
