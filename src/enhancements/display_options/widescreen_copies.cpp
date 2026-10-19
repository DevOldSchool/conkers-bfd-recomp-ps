// The pause menu's background in widescreen: the paused scene at its full width.
// Native helpers called from the [[patches.hook]] snippets in config/us.toml (func_151D61B0,
// func_151D5E90).
//
// When the pause menu (func_150198FC) opens it copies the last frame into the Z-buffer with
// func_151D5E90, a whole-frame copy drawn as texture rectangles, then blurs that copy on the CPU
// with func_151D61B0 a little more each frame for its first few frames, and draws it as the
// background every frame with func_151D5E90 again.
//
// RT64 renders the copy from its own frame, at the full width of a widescreen picture, and draws
// the background from that while it lasts. But the CPU blur changes the copy in RDRAM, where it is
// only the 292x216 4:3 middle of the frame, so RT64 drops its own copy and the background could
// only be that middle, stretched or with nothing at the sides. In widescreen the blur is skipped,
// so the background is the paused scene itself, sharp, at its full width and its own shape. In 4:3
// the game blurs it as on an N64.
//
// func_151D5E90 draws in 128x16 pieces, three to a row (128, 128 and the last 36 pixels). RT64
// copies each piece from its own frame at a horizontal scale that is not a whole number in
// widescreen, and the middle piece came out a fraction of a pixel to the side of the others, which
// showed as a slight stretch at its edges. In widescreen its pieces are replaced by full-width
// strips 7 lines tall (292x7 RGBA16 is 4088 bytes, within TMEM's 4 KiB), so there are no joins
// across a row; the vertical scale is a whole number, so the rows meet exactly.
#include <cstdint>
#include <vector>

#include "recomp.h"
#include "platform/display_mode.hpp"

namespace {

struct Command {
    uint32_t w0, w1;
};

constexpr uint32_t kSetTextureImage = 0xFD;
constexpr uint32_t kRdpHalf2 = 0xF1;
constexpr uint32_t kTexRect = 0xE4;
constexpr uint32_t kTmemBytes = 4096;

bool g_rewriting = false;
gpr g_start = 0;  // where func_151D5E90 started writing

uint32_t op(const Command& command) { return command.w0 >> 24; }

// Full-width strips for the image `image` (FD command) of width x height, in the order the game
// issues each piece's commands.
void add_strips(std::vector<Command>& out, const Command& image, uint32_t width, uint32_t height) {
    const uint32_t row_bytes = width * 2;
    const uint32_t lines = kTmemBytes / row_bytes;
    const uint32_t line_words = (row_bytes + 7) / 8;
    const uint32_t set_tile = 0xF5100000 | ((line_words & 0x1FF) << 9);  // RGBA16, TMEM 0
    const uint32_t lrs = (width - 1) << 2;
    for (uint32_t y = 0; y < height; y += lines) {
        const uint32_t rows = height - y < lines ? height - y : lines;
        const uint32_t ult = y << 2;
        const uint32_t lrt = (y + rows - 1) << 2;
        out.push_back(image);
        out.push_back({set_tile, 0x07000000});                    // G_TX_LOADTILE
        out.push_back({0xE6000000, 0});                           // G_RDPLOADSYNC
        out.push_back({0xF4000000 | ult, 0x07000000 | (lrs << 12) | lrt});  // G_LOADTILE
        out.push_back({0xE7000000, 0});                           // G_RDPPIPESYNC
        out.push_back({set_tile, 0});                             // tile 0
        out.push_back({0xF2000000 | ult, (lrs << 12) | lrt});     // G_SETTILESIZE
        out.push_back({0xE4000000 | ((width << 2) << 12) | ((y + rows) << 2), ult});  // G_TEXRECT
        out.push_back({0xE1000000, y << 5});                      // S 0, T y
        out.push_back({0xF1000000, 0x04000400});                  // DSDX, DTDY 1
        out.push_back({0xE7000000, 0});
    }
}

} // namespace

// At func_151D61B0's entry: nonzero to return without blurring.
extern "C" int conker_widescreen_skip_blur() {
    return conker::display::widescreen(conker::display::current()) ? 1 : 0;
}

// At func_151D5E90's entry: $a0 is the display list pointer it writes from.
extern "C" void conker_widescreen_copy_begin(uint8_t* /*rdram*/, recomp_context* ctx) {
    g_rewriting = conker::display::widescreen(conker::display::current());
    g_start = ctx->r4;
}

// Before func_151D5E90 returns: $a0 and $v0 both hold the display list pointer past its commands.
// The set-up before the first piece and the restore after the last are kept; the pieces between
// become strips. Anything unexpected leaves the list as the game wrote it.
extern "C" void conker_widescreen_copy_end(uint8_t* rdram, recomp_context* ctx) {
    if (!g_rewriting) {
        return;
    }
    g_rewriting = false;
    std::vector<Command> commands;
    for (gpr address = g_start; address < ctx->r4; address += 8) {
        commands.push_back({static_cast<uint32_t>(MEM_W(0x0, address)), static_cast<uint32_t>(MEM_W(0x4, address))});
    }
    size_t first = commands.size();
    size_t last = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    for (size_t i = 0; i < commands.size(); i++) {
        if (op(commands[i]) == kSetTextureImage && first == commands.size()) {
            first = i;
            width = (commands[i].w0 & 0xFFF) + 1;
        }
        if (op(commands[i]) == kTexRect) {
            const uint32_t lry = (commands[i].w0 & 0xFFF) >> 2;
            height = lry > height ? lry : height;
        }
        if (op(commands[i]) == kRdpHalf2) {
            last = i;
        }
    }
    // The pieces end with G_RDPHALF_2 and a pipe sync.
    if (first == commands.size() || last <= first || last + 1 >= commands.size() || width == 0 || height == 0 ||
        width * 2 > kTmemBytes || ((commands[first].w0 >> 19) & 0x3) != 2) {
        return;
    }
    std::vector<Command> out(commands.begin(), commands.begin() + static_cast<long>(first));
    add_strips(out, commands[first], width, height);
    out.insert(out.end(), commands.begin() + static_cast<long>(last + 2), commands.end());
    if (out.size() > commands.size() + 64) {
        return;  // more than the game's own list by a margin: leave it alone
    }
    gpr address = g_start;
    for (const Command& command : out) {
        MEM_W(0x0, address) = static_cast<int32_t>(command.w0);
        MEM_W(0x4, address) = static_cast<int32_t>(command.w1);
        address += 8;
    }
    ctx->r4 = address;
    ctx->r2 = address;
}
