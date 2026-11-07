// The speech and thought bubbles across the whole widened picture. Native helpers called from the
// [[patches.hook]] snippets in config/us.toml (func_1516B6BC, func_15095D34).
//
// func_1516B6BC draws a bubble: its 64x64 picture (and the shadow, the same picture in black) is
// stretched over the text in pieces, each a texture rectangle written by func_15095D34 from the
// rectangle func_15095B08 left in D_800D2C78 (left), D_800D2C7C (top), D_800D2C84 (width) and
// D_800D2C88 (height), in pixels of the 292x216 picture. A wide bubble reaches past the picture's
// sides, and a TEXRECT cannot start left of x 0, so func_15095D34 clamps the left side to 0 and moves
// the texture along to match; the right side is left to the scissor. On an N64 both are the screen's
// edges. In widescreen RT64 keeps rectangles in the 4:3 middle, so the bubble stopped in a straight
// line at each 4:3 side with the scene showing past it.
//
// In widescreen, while a bubble is drawn, the rectangle's left is moved kShift pixels to the right
// before func_15095D34 writes it, so nothing is clamped; its TEXRECT is then replaced by an RT64
// extended rectangle kShift pixels back to the left, placed from the picture's centre (so it may go
// past the 4:3 sides), under the game's scissor stretched to the widened picture (as for the turret's
// frame, widescreen_frame.cpp: a scissor of our own would change how RT64 treats the whole frame).
#include <cstdint>
#include <cstring>

#include "recomp.h"
#include "platform/display_mode.hpp"

namespace {

constexpr gpr kRectLeft = static_cast<gpr>(static_cast<int32_t>(0x800D2C78));  // D_800D2C78, f32
// Far enough that a bubble's left never goes below 0, near enough that its right stays inside a
// TEXRECT's 10.2 coordinates (under 1024).
constexpr float kShift = 256.0f;
constexpr int kShiftQuarters = static_cast<int>(kShift) * 4;
constexpr int kCentreQuarters = 146 * 4;  // the 292-wide picture's centre, the extended origin

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
uint32_t halves(int high, int low) {
    return (static_cast<uint32_t>(high & 0xFFFF) << 16) | static_cast<uint32_t>(low & 0xFFFF);
}

bool g_in_bubble = false;  // inside func_1516B6BC, in widescreen
bool g_shifted = false;    // the current func_15095D34 call's rectangle was moved
gpr g_start = 0;           // where that call started writing

float read_f32(uint8_t* rdram, gpr address) {
    const uint32_t bits = static_cast<uint32_t>(MEM_W(0x0, address));
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void write_f32(uint8_t* rdram, gpr address, float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    MEM_W(0x0, address) = static_cast<int32_t>(bits);
}

} // namespace

// At func_1516B6BC's entry and before it returns.
extern "C" void conker_widescreen_bubble_begin(uint8_t*, recomp_context*) {
    g_in_bubble = conker::display::logic_widescreen();
}

extern "C" void conker_widescreen_bubble_end(uint8_t*, recomp_context*) {
    g_in_bubble = false;
}

// At func_15095D34's entry: $a0 is the display list pointer it writes from.
extern "C" void conker_widescreen_bubble_rect_begin(uint8_t* rdram, recomp_context* ctx) {
    g_shifted = false;
    if (!g_in_bubble) {
        return;
    }
    const float left = read_f32(rdram, kRectLeft);
    if (!(left > -kShift && left < 1023.0f - kShift)) {
        return;
    }
    write_f32(rdram, kRectLeft, left + kShift);
    g_shifted = true;
    g_start = ctx->r4;
}

// Before func_15095D34 returns: $v0 holds the display list pointer past its commands, which end in
// G_TEXRECT, G_RDPHALF_1, G_RDPHALF_2 (after an optional G_SETPRIMCOLOR).
extern "C" void conker_widescreen_bubble_rect_end(uint8_t* rdram, recomp_context* ctx) {
    if (!g_shifted) {
        return;
    }
    g_shifted = false;
    write_f32(rdram, kRectLeft, read_f32(rdram, kRectLeft) - kShift);

    const gpr rect = ctx->r2 - 24;
    if (rect < g_start) {
        return;
    }
    const uint32_t w0 = static_cast<uint32_t>(MEM_W(0x0, rect));
    const uint32_t w1 = static_cast<uint32_t>(MEM_W(0x4, rect));
    const uint32_t half1 = static_cast<uint32_t>(MEM_W(0x8, rect));
    const uint32_t st = static_cast<uint32_t>(MEM_W(0xC, rect));
    const uint32_t half2 = static_cast<uint32_t>(MEM_W(0x10, rect));
    const uint32_t steps = static_cast<uint32_t>(MEM_W(0x14, rect));
    if ((w0 >> 24) != kTexRect || (half1 >> 24) != kRdpHalf1 || (half2 >> 24) != kRdpHalf2) {
        return;
    }
    const int lrx = static_cast<int>((w0 >> 12) & 0xFFF) - kShiftQuarters;
    const int lry = static_cast<int>(w0 & 0xFFF);
    const uint32_t tile = (w1 >> 24) & 0x7;
    const int ulx = static_cast<int>((w1 >> 12) & 0xFFF) - kShiftQuarters;
    const int uly = static_cast<int>(w1 & 0xFFF);

    const uint32_t out[][2] = {
        {kHook, kEnable},
        {extended(kExSetScissorAspect), kAspectStretch},
        {extended(kExTexRect), tile | (kOriginCenter << 3) | (kOriginCenter << 15)},
        {halves(ulx - kCentreQuarters, uly), halves(lrx - kCentreQuarters, lry)},
        {st, steps},
        {extended(kExSetScissorAspect), kAspectAuto},
        {kHook, kDisable},
    };
    gpr address = rect;
    for (const auto& command : out) {
        MEM_W(0x0, address) = static_cast<int32_t>(command[0]);
        MEM_W(0x4, address) = static_cast<int32_t>(command[1]);
        address += 8;
    }
    ctx->r2 = address;
}
