// Whole-frame copies in widescreen: the pause menu's blurred background reaches both edges.
// Native helpers called from the [[patches.hook]] snippets in config/us.toml (func_151D5E90).
//
// func_151D5E90(gfx, image, ...) draws a frame-sized RGBA16 image over the whole picture, as
// 128x16 texture rectangles in rows of three (128, 128 and the last 36 pixels). The pause menu
// (func_150198FC) uses it to copy the last frame into the Z-buffer, which it then blurs on the CPU
// (func_151D61B0), and to draw that saved frame as its background every frame; func_151D6778 uses
// it for another whole-frame copy.
//
// In widescreen RT64 keeps 2D rectangles in the 4:3 area unless they span the scissor's width or
// read a copy of a frame it rendered. The copy of the last frame does, so it is stretched across
// the widened picture, but the saved frame is read back from RDRAM once the CPU has blurred it, so
// its rectangles stayed in the middle, with the stretched copy left over at the sides behind a
// seam. Around each call the display list now gets RT64's extended GBI commands to stretch the
// call's rectangles to the whole width (gEXSetRectAspect, include/rt64_extended_gbi.h in lib/rt64)
// and to turn the extended GBI off again. RT64 has only the 292x216 image for the saved frame, so
// it is the 4:3 frame stretched; it is blurred, so it shows little. In 4:3 nothing is added.
#include <cstdint>

#include "recomp.h"
#include "platform/display_mode.hpp"

namespace {

// RT64's extended GBI. The hook command is F3DEX2's G_SPNOOP with RT64's magic number; it
// enables the extended commands under opcode 0x64, which F3DEX2CBFD does not use.
constexpr uint32_t kHook = 0xE0525464;                // G_SPNOOP, "RT" 0x64
constexpr uint32_t kHookEnable = 0x10000064;          // RT64_HOOK_OP_ENABLE, opcode 0x64
constexpr uint32_t kHookDisable = 0x20000000;         // RT64_HOOK_OP_DISABLE
constexpr uint32_t kSetRectAspect = 0x64000033;       // G_EX_SETRECTASPECT_V1
constexpr uint32_t kAspectAuto = 0;                   // G_EX_ASPECT_AUTO
constexpr uint32_t kAspectStretch = 1;                // G_EX_ASPECT_STRETCH

// Writes two commands at the display list pointer in $a0 and moves it past them.
void emit(uint8_t* rdram, recomp_context* ctx, uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3) {
    MEM_W(0x0, ctx->r4) = static_cast<int32_t>(w0);
    MEM_W(0x4, ctx->r4) = static_cast<int32_t>(w1);
    MEM_W(0x8, ctx->r4) = static_cast<int32_t>(w2);
    MEM_W(0xC, ctx->r4) = static_cast<int32_t>(w3);
    ctx->r4 += 0x10;
}

bool g_stretching = false;  // the commands before the call were written, so the ones after are due

} // namespace

// At func_151D5E90's entry: $a0 is the display list pointer it writes from.
extern "C" void conker_widescreen_copy_begin(uint8_t* rdram, recomp_context* ctx) {
    g_stretching = conker::display::widescreen(conker::display::current());
    if (g_stretching) {
        emit(rdram, ctx, kHook, kHookEnable, kSetRectAspect, kAspectStretch);
    }
}

// Before func_151D5E90 returns: $a0 and $v0 both hold the display list pointer past its commands.
extern "C" void conker_widescreen_copy_end(uint8_t* rdram, recomp_context* ctx) {
    if (g_stretching) {
        g_stretching = false;
        emit(rdram, ctx, kSetRectAspect, kAspectAuto, kHook, kHookDisable);
        ctx->r2 = ctx->r4;
    }
}
