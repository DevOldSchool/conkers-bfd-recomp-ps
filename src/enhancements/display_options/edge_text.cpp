// HUD text placed from the left edge of the widened picture instead of the 4:3 one, for mods: text
// queued with flag 0x20 (HUD_TEXT_LEFT_EDGE in mods/include/conker.h, which the game never sets) is
// drawn with its x counted from the window's left side. Native helpers called from the
// [[patches.hook]] snippets in config/us.toml (func_15043384).
//
// func_15043384 draws the frame's queued text, one node at a time, into the display list in $s2:
// func_150417AC writes each line's glyphs (and its shadow) as TEXRECTs. Before a run of such nodes
// RT64's extended GBI moves rectangles left by as much as the picture is widened on each side, under
// a scissor stretched to the whole picture (the game's own, x 2 to 290, would cut them off at the
// 4:3 side); after the run both are put back. RT64's LEFT origin would do it, but it sits at the
// screen's side only with userConfig.extAspectRatio = Expand, which the renderer leaves Original, so
// the rectangles keep the CENTER origin and an offset instead. In 4:3 nothing is added.
#include <cmath>
#include <cstdint>

#include "recomp.h"
#include "platform/display_mode.hpp"

namespace {

constexpr float kPictureWidth = 292.0f;
constexpr float kPictureAspect = kPictureWidth / 216.0f;
constexpr float kUndrawnColumns = 2.0f;  // the frame buffer's columns left out at each side

// A text node (Game70200TextNode).
constexpr int kNodeFlags = 0x0C;
constexpr int kNodeKind = 0x0D;  // 0 for text, else a picture
constexpr int kNodeNext = 0x18;
constexpr uint8_t kLeftEdge = 0x20;

// RT64's extended GBI (lib/rt64/include/rt64_extended_gbi.h), through F3DEX2's G_SPNOOP.
constexpr uint32_t kHook = 0xE0525464;                      // RT64_HOOK_OPCODE, RT64_HOOK_MAGIC_NUMBER
constexpr uint32_t kExtendedOpcode = 0x64;                  // RT64_EXTENDED_OPCODE
constexpr uint32_t kEnable = 0x10000000 | kExtendedOpcode;  // RT64_HOOK_OP_ENABLE
constexpr uint32_t kDisable = 0x20000000;                   // RT64_HOOK_OP_DISABLE
constexpr uint32_t kExSetRectAlign = 0x000006;              // G_EX_SETRECTALIGN_V1
constexpr uint32_t kExSetScissorAspect = 0x000034;          // G_EX_SETSCISSORASPECT_V1
constexpr uint32_t kOriginNone = 0x800;                     // G_EX_ORIGIN_NONE
constexpr uint32_t kOriginCenter = 0x200;                   // G_EX_ORIGIN_CENTER
constexpr uint32_t kAspectAuto = 0x0;                       // G_EX_ASPECT_AUTO
constexpr uint32_t kAspectStretch = 0x1;                    // G_EX_ASPECT_STRETCH

uint32_t extended(uint32_t command) { return (kExtendedOpcode << 24) | command; }

bool g_open = false;  // the offset is set in the list being written

bool on_left_edge(uint8_t* rdram, gpr node) {
    return node != 0 && (MEM_BU(kNodeFlags, node) & kLeftEdge) != 0 && MEM_BU(kNodeKind, node) == 0;
}

// How far the widened picture's visible left side is past the 4:3 one, in 4:3 pixels (as the turret
// frame lays it out: src/enhancements/display_options/widescreen_frame.cpp).
float widened_by() {
    const float widening = conker::display::logic_aspect() / kPictureAspect;
    return (kPictureWidth / 2.0f - kUndrawnColumns) * (widening - 1.0f);
}

void write(uint8_t* rdram, gpr& dl, uint32_t w0, uint32_t w1) {
    MEM_W(0x0, dl) = static_cast<int32_t>(w0);
    MEM_W(0x4, dl) = static_cast<int32_t>(w1);
    dl += 8;
}

// G_EX_SETRECTALIGN_V1 with both sides from `origin`, moved `offset` 4:3 pixels to the right.
void write_align(uint8_t* rdram, gpr& dl, uint32_t origin, float offset) {
    const uint32_t quarters = static_cast<uint32_t>(static_cast<int>(std::lround(offset * 4.0f))) & 0xFFFF;
    write(rdram, dl, extended(kExSetRectAlign), origin | (origin << 12));
    write(rdram, dl, quarters << 16, quarters << 16);
}

}  // namespace

// Before a visible text node is drawn: $s0 the node, $s2 the display list.
extern "C" void conker_edge_text_begin(uint8_t* rdram, recomp_context* ctx) {
    if (g_open || !on_left_edge(rdram, ctx->r16)) {
        return;
    }
    const float by = widened_by();
    if (by < 0.5f) {
        return;
    }
    gpr dl = ctx->r18;
    write(rdram, dl, kHook, kEnable);
    // A CENTER-aligned rectangle's x counts from the middle of the picture.
    write_align(rdram, dl, kOriginCenter, -kPictureWidth / 2.0f - by);
    write(rdram, dl, extended(kExSetScissorAspect), kAspectStretch);
    write(rdram, dl, kHook, kDisable);
    ctx->r18 = dl;
    g_open = true;
}

// After it is drawn: $v0 the display list past it. The run ends before a node of the game's own.
extern "C" void conker_edge_text_end(uint8_t* rdram, recomp_context* ctx) {
    if (!g_open || on_left_edge(rdram, static_cast<gpr>(MEM_W(kNodeNext, ctx->r16)))) {
        return;
    }
    gpr dl = ctx->r2;
    write(rdram, dl, kHook, kEnable);
    write(rdram, dl, extended(kExSetScissorAspect), kAspectAuto);
    write_align(rdram, dl, kOriginNone, 0.0f);
    write(rdram, dl, kHook, kDisable);
    ctx->r2 = dl;
    g_open = false;
}
