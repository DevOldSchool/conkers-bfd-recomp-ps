// The level's visibility table, for the player as well as the camera. A native helper called from the
// [[patches.hook]] snippet in config/us.toml (func_1510FC34).
//
// Each frame func_1510FC34 finds, for each view, the cell of the level's quadtree (the root at
// D_800DBE48, 512-unit leaves) that the camera stands in, keeps its index in D_800DBE30[view], and when
// it changes func_150A6760 decodes that cell's row of the visibility table into the nodes: bit `view`
// of each node's +0 halfword set hides it, and func_150A5378 skips it when it draws the level. The table
// is made for the cells, not for every point in them, and the camera can stand in a cell behind the one
// the player looks from: on Beach (scene 43) the camera of a player standing in the high room with the
// lightning-bolt walls crosses into the cell by its other door, whose row hides the dark corridor
// beyond the doorway, and the sky showed through the doorway instead.
//
// So a node is hidden only when the rows of both cells hide it: the camera's and the one the view's
// player stands in. Both rows are decoded every frame from the table (the game decodes only on a
// change), so the nodes' bits depend on nothing but the two cells, and nothing is kept between frames.
//
// The rows were also made for a 4:3 view. In widescreen the wider sides of the view reach what the
// camera's cell's row hides: from inside the hollow log on Windy (scene 6) the camera looks past the
// log's end, where the hidden hillside left black with bits of grass floating in it. In widescreen,
// while the camera follows its player, the rows of the eight cells around the camera count too, so a
// node is hidden only when every one of them hides it; what the view test leaves out still is not
// drawn (widescreen_culling.cpp).
// Only drawing changes. CONKER_LEVEL_VISIBILITY=0 keeps the game's own.
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "recomp.h"
#include "platform/display_mode.hpp"

extern "C" void func_1510FD20(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_1510FE30(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_150A6760(uint8_t* rdram, recomp_context* ctx);

namespace {

inline gpr kseg0(uint32_t address) { return static_cast<gpr>(static_cast<int32_t>(address)); }

constexpr uint32_t kTreeRoot = 0x800DBE48;      // D_800DBE48: the level's quadtree
constexpr uint32_t kViewCells = 0x800DBE30;     // D_800DBE30: s16 per view, the cell whose row is decoded
constexpr uint32_t kViewRecords = 0x800DBFF0;   // D_800DBFF0: the views' camera records
constexpr uint32_t kViewRecordSize = 0x9A0;
constexpr uint32_t kActors = 0x800CC2D0;        // D_800CC2D0: player n is actor n
constexpr uint32_t kActorSize = 0x32C;
constexpr uint32_t kActorViewRecord = 0x318;    // a player's camera record
constexpr uint32_t kActorX = 0x14;              // f32 x, y, z
constexpr uint32_t kActorZ = 0x1C;
constexpr uint32_t kRecordCameraX = 0x2F8;     // f32: the camera's x and z, as func_1510FC34 reads them
constexpr uint32_t kRecordCameraZ = 0x300;
constexpr int kPlayers = 4;
// A following camera stays well within two of the tree's 512-unit leaves of its player.
constexpr float kFollowDistance = 1024.0f;
constexpr float kCell = 512.0f;  // the tree's leaves
constexpr uint32_t kNodeHidden = 0x0;           // s16: a hidden bit per view (and the box's bottom)
constexpr uint32_t kNodeNext = 0x4;             // s16: offset to the next sibling, 0 for the last
constexpr uint32_t kNodeChild = 0xC;            // s16: offset to the first child, 0 for a leaf
constexpr size_t kMaxNodes = 65536;

const bool g_enabled = [] {
    const char* value = std::getenv("CONKER_LEVEL_VISIBILITY");
    return value == nullptr || value[0] != '0';
}();

float load_float(uint8_t* rdram, uint32_t address) {
    const int32_t word = MEM_W(0, kseg0(address));
    float value;
    std::memcpy(&value, &word, sizeof(value));
    return value;
}

bool in_rdram(uint32_t address) { return (address & 0xFF800000) == 0x80000000; }

// The player whose camera record is this view's, or 0.
uint32_t view_player(uint8_t* rdram, int view) {
    const uint32_t records = static_cast<uint32_t>(MEM_W(0, kseg0(kViewRecords)));
    if (!in_rdram(records)) {
        return 0;
    }
    const uint32_t record = records + static_cast<uint32_t>(view) * kViewRecordSize;
    for (int n = 0; n < kPlayers; n++) {
        const uint32_t actor = kActors + static_cast<uint32_t>(n) * kActorSize;
        if (MEM_W(0, kseg0(actor)) != 0 && static_cast<uint32_t>(MEM_W(0, kseg0(actor + kActorViewRecord))) == record) {
            return actor;
        }
    }
    return 0;
}

// The index of the cell at (x, z), as func_1510FC34 finds the camera's, or -1 outside the tree.
int cell_at(uint8_t* rdram, recomp_context* ctx, int32_t x, int32_t z) {
    ctx->r4 = x;
    ctx->r5 = z;
    func_1510FD20(rdram, ctx);
    if (ctx->r2 == 0) {
        return -1;
    }
    ctx->r4 = ctx->r2;
    func_1510FE30(rdram, ctx);
    return static_cast<int16_t>(ctx->r2);
}

// Every node of the tree.
void collect_nodes(uint8_t* rdram, std::vector<uint32_t>* nodes) {
    nodes->clear();
    std::vector<uint32_t> pending;
    pending.push_back(static_cast<uint32_t>(MEM_W(0, kseg0(kTreeRoot))));
    while (!pending.empty() && nodes->size() < kMaxNodes) {
        uint32_t node = pending.back();
        pending.pop_back();
        while (in_rdram(node) && nodes->size() < kMaxNodes) {
            nodes->push_back(node);
            const int16_t child = MEM_H(kNodeChild, kseg0(node));
            if (child != 0) {
                pending.push_back(node + child);
            }
            const int16_t next = MEM_H(kNodeNext, kseg0(node));
            if (next == 0) {
                break;
            }
            node += next;
        }
    }
}

// Decodes `cell`'s row into the nodes' bits for `view`, as func_1510FC34 does on a change.
void decode_row(uint8_t* rdram, recomp_context* ctx, int view, int cell) {
    MEM_H(0, kseg0(kViewCells + 2 * view)) = static_cast<int16_t>(cell);
    ctx->r4 = view;
    func_150A6760(rdram, ctx);
}

}  // namespace

// At 0x1510FCF0, once func_1510FC34 has the camera's cell for view $s0 in D_800DBE30[view] (and its row
// decoded if it changed).
extern "C" void conker_level_visibility(uint8_t* rdram, recomp_context* ctx) {
    if (!g_enabled || !in_rdram(static_cast<uint32_t>(MEM_W(0, kseg0(kTreeRoot))))) {
        return;
    }
    const int view = static_cast<int>(ctx->r16);
    if (view < 0 || view >= kPlayers) {
        return;
    }
    const uint32_t player = view_player(rdram, view);
    if (player == 0) {
        return;
    }
    const recomp_context saved = *ctx;
    const int camera_cell = MEM_H(0, kseg0(kViewCells + 2 * view));
    // Only while the camera follows its player. A cutscene's camera can be in another room: Heist's
    // briefing is shot in the boss's office while the players wait in their corridors, over 1,800
    // units away, and the corridors' rows showed some 12,800 more nodes there, after which the
    // office's floor was not drawn and the characters left trails across it. Far from the player the
    // camera's row stands alone (decoded again below, in case last frame's bits were a union).
    const uint32_t record = static_cast<uint32_t>(MEM_W(0, kseg0(kViewRecords))) + static_cast<uint32_t>(view) * kViewRecordSize;
    const float dx = load_float(rdram, record + kRecordCameraX) - load_float(rdram, player + kActorX);
    const float dz = load_float(rdram, record + kRecordCameraZ) - load_float(rdram, player + kActorZ);
    const bool follows = dx * dx + dz * dz <= kFollowDistance * kFollowDistance;
    const int player_cell = !follows ? camera_cell
                                     : cell_at(rdram, ctx, static_cast<int32_t>(load_float(rdram, player + kActorX)),
                                               static_cast<int32_t>(load_float(rdram, player + kActorZ)));
    if (player_cell >= 0 && camera_cell >= 0) {
        // The cells whose rows count: the camera's, the player's and, in widescreen, the camera's
        // neighbours (see the top of the file).
        int cells[11];
        int count = 0;
        auto add = [&](int cell) {
            if (cell < 0) {
                return;
            }
            for (int i = 0; i < count; i++) {
                if (cells[i] == cell) {
                    return;
                }
            }
            cells[count++] = cell;
        };
        add(camera_cell);
        add(player_cell);
        if (follows && conker::display::logic_widescreen()) {
            const float cx = load_float(rdram, record + kRecordCameraX);
            const float cz = load_float(rdram, record + kRecordCameraZ);
            for (int ix = -1; ix <= 1; ix++) {
                for (int iz = -1; iz <= 1; iz++) {
                    if (ix != 0 || iz != 0) {
                        add(cell_at(rdram, ctx, static_cast<int32_t>(cx + kCell * ix), static_cast<int32_t>(cz + kCell * iz)));
                    }
                }
            }
        }
        static std::vector<uint32_t> nodes;
        static std::vector<uint8_t> hidden;
        collect_nodes(rdram, &nodes);
        const int16_t bit = static_cast<int16_t>(1 << view);
        // The camera's row, as the game has it (decoded again: the bits may be last frame's union).
        decode_row(rdram, ctx, view, camera_cell);
        if (count > 1) {
            hidden.resize(nodes.size());
            for (size_t i = 0; i < nodes.size(); i++) {
                hidden[i] = (MEM_H(kNodeHidden, kseg0(nodes[i])) & bit) != 0;
            }
            for (int c = 1; c < count; c++) {
                decode_row(rdram, ctx, view, cells[c]);
                for (size_t i = 0; i < nodes.size(); i++) {
                    hidden[i] = hidden[i] && (MEM_H(kNodeHidden, kseg0(nodes[i])) & bit) != 0;
                }
            }
            for (size_t i = 0; i < nodes.size(); i++) {
                const int16_t value = MEM_H(kNodeHidden, kseg0(nodes[i]));
                MEM_H(kNodeHidden, kseg0(nodes[i])) = static_cast<int16_t>(hidden[i] ? (value | bit) : (value & ~bit));
            }
            MEM_H(0, kseg0(kViewCells + 2 * view)) = static_cast<int16_t>(camera_cell);
        }
    }
    *ctx = saved;
}
