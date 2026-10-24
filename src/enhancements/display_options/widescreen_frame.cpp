// The metal frame of the turret's first-person view (Windy's hive turret, and inside the hive in
// scene 39) across the whole widened picture. Native helpers called from the [[patches.hook]] snippets in
// config/us.toml (func_15154D80).
//
// The frame is 2D sprites: a plate along the top and the same plate flipped along the bottom, with
// eyecups between them. func_15154D80 draws a sprite record ($a1) into the display list at $a0.
// A plate is four 32x32 RGBA32 tiles, sprite textures 181 to 184 in D_80090B60 (byte +0x20 of the
// record; resources 2810 to 2813), each stretched over a texture rectangle 71.5 pixels wide, from
// x 3 to 289: the curled left end, two middle pieces with a bolt each, and the curled right end.
// Scene 39 draws them one tile to a sprite record, right to left; each rectangle is dealt with on
// its own, so several tiles in one call would do as well.
// In widescreen RT64 keeps rectangles in the 4:3 middle, so the plate stopped short of the sides
// with the scene showing past its ends. Even in 4:3 the game leaves a pixel of the scene past each
// end, between its scissor's sides (x 2 and 290) and the plate's.
//
// Widened, the plate is laid out again from the left edge of the picture to the right one, with
// the same ends and as many middle pieces as fit at about their own width: extra copies of the left
// middle piece on the left and of the right one on the right, so the bolts carry on at their
// spacing. The plate's lip is lit brighter towards the right across the middle pieces, so a piece
// followed by a copy of itself would show a step in shade; every second copy is mirrored instead,
// so each join meets the same column of the tile on both sides. A run of copies then has to start
// and end the right way round to meet the tiles beside it, so each run has an odd number of copies.
// Each tile's rectangle is replaced by its pieces, drawn with the texture the game has just loaded
// for it, as RT64 extended rectangles placed from the picture's centre in 4:3 pixels, so they go
// past the 4:3 sides, under a scissor opened to the whole picture. In 4:3 the same lays the four
// tiles out from x 2 to 290, each half a pixel wider.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "recomp.h"
#include "platform/display_mode.hpp"

namespace {

struct Command {
    uint32_t w0, w1;
};

constexpr int kSpriteTexture = 0x20;         // sprite record: index into the sprite texture table D_80090B60
constexpr uint8_t kFirstPlateTexture = 181;  // the plate's tiles, left to right
constexpr uint8_t kLastPlateTexture = 184;
constexpr int kTiles = 4;
constexpr int kTileLeft = 3 * 4;  // the plate's tiles in 10.2 fixed point: the first starts at x 3
constexpr int kTileWidth = 286;   // and each is 71.5 pixels wide
constexpr int kSlack = 2;         // a quarter pixel either way of rounding
constexpr float kPictureWidth = 292.0f;
constexpr float kPictureHeight = 216.0f;
constexpr float kPictureCentre = kPictureWidth / 2.0f;
constexpr float kUndrawnColumns = 2.0f;  // the frame buffer's columns left out at each side
// RT64 snaps an extended rectangle's sides leftwards to a whole N64 pixel of the frame buffer and
// its misalignment, which left a sliver of the scene past the right end; that end is drawn this many
// 4:3 pixels further (the left one only goes past the left side).
constexpr float kSnapAllowance = 2.0f;
constexpr float kPiece = kTileWidth / 4.0f;
constexpr int kMaxPieces = 16;

constexpr uint32_t kTexRect = 0xE4;
constexpr uint32_t kRdpHalf1 = 0xE1;
constexpr uint32_t kRdpHalf2 = 0xF1;

// RT64's extended GBI (lib/rt64/include/rt64_extended_gbi.h), through F3DEX2's G_SPNOOP.
constexpr uint32_t kHook = 0xE0525464;                      // RT64_HOOK_OPCODE, RT64_HOOK_MAGIC_NUMBER
constexpr uint32_t kExtendedOpcode = 0x64;                  // RT64_EXTENDED_OPCODE
constexpr uint32_t kEnable = 0x10000000 | kExtendedOpcode;  // RT64_HOOK_OP_ENABLE
constexpr uint32_t kDisable = 0x20000000;                   // RT64_HOOK_OP_DISABLE
constexpr uint32_t kExTexRect = 0x000002;                   // G_EX_TEXRECT_V1
constexpr uint32_t kExSetScissorAspect = 0x000034;          // G_EX_SETSCISSORASPECT_V1
constexpr uint32_t kOriginCenter = 0x200;                   // G_EX_ORIGIN_CENTER
constexpr uint32_t kAspectAuto = 0x0;                       // G_EX_ASPECT_AUTO
constexpr uint32_t kAspectStretch = 0x1;                    // G_EX_ASPECT_STRETCH

uint32_t extended(uint32_t command) { return (kExtendedOpcode << 24) | command; }
uint32_t op(const Command& command) { return command.w0 >> 24; }
uint32_t halves(int high, int low) {
    return (static_cast<uint32_t>(high & 0xFFFF) << 16) | static_cast<uint32_t>(low & 0xFFFF);
}
int to_quarters(float x) { return static_cast<int>(std::lround(x * 4.0f)); }

bool g_rewriting = false;
gpr g_start = 0;  // where func_15154D80 started writing

// The widened plate: pieces of equal width from `from` to `to` (4:3 pixels from the left of the 4:3
// picture), the first and last the ends and the middle ones split between the two middle tiles in
// two runs of an odd length.
struct Layout {
    float from, to;
    int pieces;
    int left_middles;

    int edge(int i) const {
        return to_quarters(from + (to - from) * static_cast<float>(i) / static_cast<float>(pieces));
    }
    // The pieces tile `slot` (0 to 3, left to right) is drawn as: [first, first + count).
    void pieces_of(int slot, int& first, int& count) const {
        const int middles = pieces - 2;
        switch (slot) {
            case 0: first = 0; count = 1; break;
            case 1: first = 1; count = left_middles; break;
            case 2: first = 1 + left_middles; count = middles - left_middles; break;
            default: first = pieces - 1; count = 1; break;
        }
    }
};

bool widened_layout(Layout& layout) {
    // RT64 widens the picture around its centre; in 4:3 pixels its visible sides are
    // kUndrawnColumns in from the frame buffer's edges, scaled by the widening.
    const float widening = conker::display::rendered_aspect() / (kPictureWidth / kPictureHeight);
    if (widening < 0.999f) {
        return false;
    }
    const float half = (kPictureCentre - kUndrawnColumns) * std::max(widening, 1.0f);
    layout.from = kPictureCentre - half;
    layout.to = kPictureCentre + half + kSnapAllowance;
    // Two odd runs make an even number of middle pieces, and so of pieces.
    const int pairs = static_cast<int>(std::lround((layout.to - layout.from) / kPiece / 2.0f));
    layout.pieces = std::clamp(pairs * 2, kTiles, kMaxPieces);
    const int middles = layout.pieces - 2;
    layout.left_middles = (middles / 2) % 2 == 1 ? middles / 2 : middles / 2 + 1;  // the longer run on the left
    return true;
}

// Which tile of the plate a rectangle from ulx to lrx (10.2) is, or -1.
int plate_slot(int ulx, int lrx) {
    if (std::abs((lrx - ulx) - kTileWidth) > kSlack) {
        return -1;
    }
    for (int slot = 0; slot < kTiles; slot++) {
        if (std::abs(ulx - (kTileLeft + slot * kTileWidth)) <= kSlack) {
            return slot;
        }
    }
    return -1;
}

// The texture rectangle `rect` (G_TEXRECT, G_RDPHALF_1, G_RDPHALF_2) as the pieces of the widened
// plate that its tile `slot` makes.
void add_pieces(std::vector<Command>& out, const Command* rect, int slot, const Layout& layout) {
    const int lrx = static_cast<int>((rect[0].w0 >> 12) & 0xFFF);
    const int lry = static_cast<int>(rect[0].w0 & 0xFFF);
    const uint32_t tile = (rect[0].w1 >> 24) & 0x7;
    const int ulx = static_cast<int>((rect[0].w1 >> 12) & 0xFFF);
    const int uly = static_cast<int>(rect[0].w1 & 0xFFF);
    const int dsdx = static_cast<int16_t>(rect[2].w1 >> 16);
    const int dtdy = static_cast<int16_t>(rect[2].w1 & 0xFFFF);
    int first, count;
    layout.pieces_of(slot, first, count);

    out.push_back({kHook, kEnable});
    // The whole picture: the game's own scissor (x 2 to 290), stretched to the widened width. A
    // scissor of its own would change what RT64 makes of the rest of the frame: it squeezes a frame
    // buffer's 2D drawing to 4:3 only while its scissors have the 4:3 shape, and stretches a fill
    // to the sides only when it spans all of them (the black of a fade, the circle wipe).
    out.push_back({extended(kExSetScissorAspect), kAspectStretch});
    // An extended coordinate counts from its origin, here the centre.
    const int centre = to_quarters(kPictureCentre);
    const int s = static_cast<int16_t>(rect[1].w1 >> 16);
    const int t = static_cast<int16_t>(rect[1].w1 & 0xFFFF);
    const int span = dsdx * (lrx - ulx) / 128;  // the texels the rectangle covers, in S's 10.5 units
    for (int piece = first; piece < first + count; piece++) {
        const int left = layout.edge(piece);
        const int right = layout.edge(piece + 1);
        // The tile's texels spread over the piece's width, every second copy mirrored.
        const int piece_dsdx = static_cast<int>(std::lround(static_cast<double>(dsdx) * (lrx - ulx) / (right - left)));
        const bool mirrored = (piece - first) % 2 == 1;
        out.push_back({extended(kExTexRect), tile | (kOriginCenter << 3) | (kOriginCenter << 15)});
        out.push_back({halves(left - centre, uly), halves(right - centre, lry)});
        out.push_back({halves(mirrored ? s + span : s, t), halves(mirrored ? -piece_dsdx : piece_dsdx, dtdy)});
    }
    out.push_back({extended(kExSetScissorAspect), kAspectAuto});
    out.push_back({kHook, kDisable});
}

} // namespace

// At func_15154D80's entry: $a0 is the display list pointer it writes from, $a1 the sprite record.
extern "C" void conker_widescreen_frame_begin(uint8_t* rdram, recomp_context* ctx) {
    g_rewriting = false;
    if (ctx->r5 == 0) {
        return;
    }
    const uint8_t texture = MEM_BU(kSpriteTexture, ctx->r5);
    g_rewriting = texture >= kFirstPlateTexture && texture <= kLastPlateTexture;
    g_start = ctx->r4;
}

// Before func_15154D80 returns: $v0 holds the display list pointer past its commands. Each of the
// plate's tile rectangles is replaced by its pieces of the widened plate; a rectangle anywhere else
// (the plate sliding in, say) is left as the game wrote it.
extern "C" void conker_widescreen_frame_end(uint8_t* rdram, recomp_context* ctx) {
    if (!g_rewriting) {
        return;
    }
    g_rewriting = false;
    Layout layout;
    if (!widened_layout(layout)) {
        return;
    }
    std::vector<Command> commands;
    for (gpr address = g_start; address < ctx->r2; address += 8) {
        commands.push_back({static_cast<uint32_t>(MEM_W(0x0, address)), static_cast<uint32_t>(MEM_W(0x4, address))});
    }
    std::vector<Command> out;
    bool changed = false;
    for (size_t i = 0; i < commands.size(); i++) {
        if (i + 2 < commands.size() && op(commands[i]) == kTexRect && op(commands[i + 1]) == kRdpHalf1 &&
            op(commands[i + 2]) == kRdpHalf2) {
            const int lrx = static_cast<int>((commands[i].w0 >> 12) & 0xFFF);
            const int ulx = static_cast<int>((commands[i].w1 >> 12) & 0xFFF);
            const int slot = plate_slot(ulx, lrx);
            if (slot >= 0) {
                add_pieces(out, &commands[i], slot, layout);
                changed = true;
                i += 2;
                continue;
            }
        }
        out.push_back(commands[i]);
    }
    if (!changed || out.size() > commands.size() + 96) {
        return;  // nothing of the plate, or more than the game's own list by a margin: leave it alone
    }
    gpr address = g_start;
    for (const Command& command : out) {
        MEM_W(0x0, address) = static_cast<int32_t>(command.w0);
        MEM_W(0x4, address) = static_cast<int32_t>(command.w1);
        address += 8;
    }
    ctx->r2 = address;
}
