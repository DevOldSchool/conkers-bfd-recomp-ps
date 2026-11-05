// The black over a multiplayer player's view once they are out of lives, across the whole widened
// view. Native helpers called from the [[patches.hook]] snippets in config/us.toml (func_15180580).
//
// func_15180580 draws a player's death wipe into the display list at $a0: black fill rectangles
// around a closing circle (quarter-circle texture rectangles), and once the circle has closed a
// single black FILLRECT over the player's viewport, from their entry in D_800BE628. A player out of
// lives keeps that black, with a skull over it (drawn after, by the caller).
//
// RT64 carries an untextured rectangle on to the side of a widened picture only when it reaches
// that side of the frame buffer's scissor, or of a widened 3D view's (rect.extendEdgeRects,
// tools/patches/rt64-split-screen-wide.patch). The viewport's black can fall short of both: in a
// two-player match the black stayed in the 4:3 middle of the top view, with the scene's sky showing
// at its widened sides. In widescreen that one FILLRECT is drawn as a stretched rectangle under a
// stretched scissor (RT64's extended rect and scissor aspect), so it covers the view as RT64 widens
// it: the whole width for a view across the screen, half of it for a quarter of the screen. The
// closing wipe is left as it is: its fills have to meet the quarter circles, which are not stretched.
#include <cstdint>
#include <vector>

#include "recomp.h"
#include "platform/display_mode.hpp"

namespace {

struct Command {
    uint32_t w0, w1;
};

constexpr uint32_t kFillRect = 0xF6;
constexpr uint32_t kTexRect = 0xE4;

// RT64's extended GBI (lib/rt64/include/rt64_extended_gbi.h), through F3DEX2's G_SPNOOP.
constexpr uint32_t kHook = 0xE0525464;                      // RT64_HOOK_OPCODE, RT64_HOOK_MAGIC_NUMBER
constexpr uint32_t kExtendedOpcode = 0x64;                  // RT64_EXTENDED_OPCODE
constexpr uint32_t kEnable = 0x10000000 | kExtendedOpcode;  // RT64_HOOK_OP_ENABLE
constexpr uint32_t kDisable = 0x20000000;                   // RT64_HOOK_OP_DISABLE
constexpr uint32_t kExSetRectAspect = 0x000033;             // G_EX_SETRECTASPECT_V1
constexpr uint32_t kExSetScissorAspect = 0x000034;          // G_EX_SETSCISSORASPECT_V1
constexpr uint32_t kAspectAuto = 0x0;                       // G_EX_ASPECT_AUTO
constexpr uint32_t kAspectStretch = 0x1;                    // G_EX_ASPECT_STRETCH

uint32_t extended(uint32_t command) { return (kExtendedOpcode << 24) | command; }
uint32_t op(const Command& command) { return command.w0 >> 24; }

bool g_rewriting = false;
gpr g_start = 0;  // where func_15180580 started writing

} // namespace

// At func_15180580's entry: $a0 is the display list pointer it writes from.
extern "C" void conker_widescreen_out_begin(uint8_t* /*rdram*/, recomp_context* ctx) {
    g_rewriting = conker::display::logic_widescreen();
    g_start = ctx->r4;
}

// Before func_15180580 returns: $v0 holds the display list pointer past its commands. A list with
// one FILLRECT and no texture rectangle is the closed wipe's black; it is wrapped in the stretched
// aspects. Anything else is left as the game wrote it.
extern "C" void conker_widescreen_out_end(uint8_t* rdram, recomp_context* ctx) {
    if (!g_rewriting) {
        return;
    }
    g_rewriting = false;
    std::vector<Command> commands;
    for (gpr address = g_start; address < ctx->r2; address += 8) {
        commands.push_back({static_cast<uint32_t>(MEM_W(0x0, address)), static_cast<uint32_t>(MEM_W(0x4, address))});
    }
    size_t fill = commands.size();
    int fills = 0;
    for (size_t i = 0; i < commands.size(); i++) {
        if (op(commands[i]) == kTexRect) {
            return;
        }
        if (op(commands[i]) == kFillRect) {
            fill = i;
            fills++;
        }
    }
    if (fills != 1 || commands.size() > 64) {
        return;
    }
    std::vector<Command> out(commands.begin(), commands.begin() + static_cast<long>(fill));
    out.push_back({kHook, kEnable});
    out.push_back({extended(kExSetRectAspect), kAspectStretch});
    out.push_back({extended(kExSetScissorAspect), kAspectStretch});
    out.push_back(commands[fill]);
    out.push_back({extended(kExSetScissorAspect), kAspectAuto});
    out.push_back({extended(kExSetRectAspect), kAspectAuto});
    out.push_back({kHook, kDisable});
    out.insert(out.end(), commands.begin() + static_cast<long>(fill + 1), commands.end());
    gpr address = g_start;
    for (const Command& command : out) {
        MEM_W(0x0, address) = static_cast<int32_t>(command.w0);
        MEM_W(0x4, address) = static_cast<int32_t>(command.w1);
        address += 8;
    }
    ctx->r2 = address;
}
