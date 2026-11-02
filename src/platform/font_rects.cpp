// The game draws each text glyph (func_150417AC) as a TEXRECT that stops short of the glyph: it
// starts partway into the first row of ink and ends partway into the last row or column, with
// the cut depending on the text's sub-pixel position. The original glyphs' blur hides it, but a
// sharp replacement from a texture pack (tools/font_pack.py) shows letters cut off. Every glyph's
// texture has an empty row above and below it and an empty column at its left (func_15015A38), so
// the rectangle is extended over those, and on the right over the column after the glyph's width;
// the texture keeps its place on screen, so only more of it shows. A rectangle the game clipped
// by more than three texels is left as it is.
//
// A glyph is recognised by its texture: a block of 224-byte buffers per font, D_80085990[bank],
// with each glyph's width in D_80085994[bank] (source width + 1).
//
// The same function draws the speech and thought bubbles' pictures ("more...", the buttons) as
// RGBA TEXRECTs one texel to a pixel, and it puts the text's sub-pixel position into where the
// texture starts: under a thought bubble "more..." starts 0.53 texels into its first row, so with
// bilinear filtering every row is blended half into the next and the word looks doubled. Such a
// rectangle (RGBA, clamped, exactly one texel to a pixel) is made to start on a whole texel, which
// moves the picture by under a pixel and draws it crisp, as it is when it lands on a whole pixel
// (in the speech bubbles). It moves toward the texture's start, so no texel is lost.
//
// Those pictures are also drawn one texel short of their tile: a 32x32 picture as 31x31 pixels,
// from its second column, and from half a row in, so its last row never shows. Most pictures have
// an empty border there, but the control stick (resources 2024-2028) fills its tile, and its base
// came out cut flat on the left and along the bottom. Such a rectangle is grown by up to a texel
// on each side to cover its whole tile, the texture keeping its place on screen.
#include "platform/font_rects.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace conker::renderer {

namespace {

constexpr uint32_t kRdramSize = 0x800000;
constexpr uint32_t kGlyphBuffers = 0x085990;  // D_80085990: per font, its glyph buffers
constexpr uint32_t kGlyphMetrics = 0x085994;  // D_80085994: per font, 4 bytes per glyph
constexpr uint32_t kGlyphBytes = 224;
constexpr uint32_t kMaxGlyphs = 128;
constexpr int kMaxFonts = 1;  // the US ROM loads one font (D_80082F80 holds one range)
constexpr int kMaxDepth = 16;
constexpr int kMaxCommands = 200000;
// Texels. Shrunk text (the legal screen draws at 1.11 texels per pixel) is cut by up to about two;
// a larger cut is left alone. Boxes clip their text with the scissor, which still applies.
constexpr double kMaxWiden = 3.0;

uint32_t read32(const uint8_t* rdram, uint32_t addr) {
    uint32_t value;
    std::memcpy(&value, rdram + addr, 4);  // rdram holds native-endian 32-bit words
    return value;
}

void write32(uint8_t* rdram, uint32_t addr, uint32_t value) {
    std::memcpy(rdram + addr, &value, 4);
}

uint8_t read8(const uint8_t* rdram, uint32_t addr) {
    return rdram[addr ^ 3];
}

struct Font {
    uint32_t glyphs = 0;   // RDRAM offset of the first glyph buffer
    uint32_t metrics = 0;  // RDRAM offset of the width table
};

struct Tile {
    uint32_t fmt = 0, siz = 0, cms = 0, cmt = 0;
    uint32_t uls = 0, ult = 0, lrs = 0, lrt = 0;  // 10.2
};

// Grows a one-to-one TEXRECT along one axis to its tile's edges, a texel at most on each side.
// `start` is the texture coordinate at the first pixel (s10.5, on a whole texel), `first` and
// `end` the rectangle's first and past-the-last pixel (10.2), `tile_first` and `tile_last` the
// tile's first and last texel (10.2).
void grow_to_tile(int32_t& start, uint32_t& first, uint32_t& end, uint32_t tile_first, uint32_t tile_last) {
    const int32_t before = start - static_cast<int32_t>(tile_first) * 8;  // s10.5
    if (before > 0 && before <= 32 && first >= static_cast<uint32_t>(before / 8)) {
        first -= before / 8;
        start -= before;
    }
    const int32_t reached = start + static_cast<int32_t>(end - first) * 8;
    const int32_t after = static_cast<int32_t>(tile_last) * 8 + 32 - reached;
    if (after > 0 && after <= 32 && end + after / 8 <= 0xFFF) {
        end += after / 8;
    }
}

// Moves one edge of a TEXRECT outward to reach `target` texels, at most kMaxWiden. `start` is the
// texture coordinate at the rectangle's first pixel (s10.5), `per_pixel` its step (s5.10).
// Returns the quarter pixels to move the edge by.
// Rounded up: shrunk text steps more than a texel a quarter pixel, and rounding down left up to half
// a texel of the first row of ink cut off (the Speedrun mod's pane, at 0.6). The texture is
// clamped and its first row and column are empty, so a start a little before 0 shows nothing more.
uint32_t widen_start(int32_t& start, int32_t per_pixel, uint32_t edge) {
    const double texels = start / 32.0;  // to the texture's first row or column, at 0
    if (texels <= 0.0 || texels > kMaxWiden || per_pixel <= 0) return 0;
    uint32_t quarters = static_cast<uint32_t>(std::ceil(texels * 1024.0 / per_pixel * 4.0));
    quarters = std::min(quarters, edge);
    start -= static_cast<int32_t>(std::lround(quarters * per_pixel / 128.0));
    return quarters;
}

// Moves a TEXRECT's top up by whole pixels to reach the texture's first row, at most kMaxWiden
// texels. When a rectangle's top is part way into a pixel, RT64 (as the RDP, whose first scanline is
// the next whole one) samples its first row a whole dtdy further into the texture (rt64_rdp.cpp,
// drawRect), so a move by part of a pixel skipped a row: the top of the R, the one glyph whose top
// stroke starts at full strength, was cut off flat. The game puts glyph tops on whole pixels, so moving
// from the whole row at or above the top keeps the texture in its place. `start` is t at the top
// (s10.5), `per_pixel` dtdy (s5.10). Returns the new top (10.2).
uint32_t widen_top(int32_t& start, int32_t per_pixel, uint32_t top) {
    const double texels = start / 32.0;
    if (texels <= 0.0 || texels > kMaxWiden || per_pixel <= 0) return top;
    const uint32_t row = top & ~3u;
    const uint32_t pixels = std::min(static_cast<uint32_t>(std::ceil(texels * 1024.0 / per_pixel)), row / 4);
    start -= static_cast<int32_t>(pixels * per_pixel / 32);
    return row - pixels * 4;
}

uint32_t widen_end(double reached, double target, int32_t per_pixel, uint32_t edge) {
    const double texels = target - reached;
    if (texels <= 0.0 || texels > kMaxWiden || per_pixel <= 0) return 0;
    const uint32_t quarters = static_cast<uint32_t>(std::ceil(texels * 1024.0 / per_pixel * 4.0));
    return std::min(quarters, 0xFFFu - edge);
}

} // namespace

void widen_font_rects(uint8_t* rdram, uint32_t display_list) {
    Font fonts[kMaxFonts];
    int font_count = 0;
    for (int i = 0; i < kMaxFonts; i++) {
        const uint32_t glyphs = read32(rdram, kGlyphBuffers + 4 * i) & 0x00FFFFFF;
        const uint32_t metrics = read32(rdram, kGlyphMetrics + 4 * i) & 0x00FFFFFF;
        if (glyphs == 0 || metrics == 0 || glyphs + kMaxGlyphs * kGlyphBytes > kRdramSize) continue;
        fonts[font_count++] = {glyphs, metrics};
    }
    if (font_count == 0) return;

    uint32_t segments[16] = {};
    uint32_t stack[kMaxDepth];
    int depth = 0;
    Tile tiles[8];
    uint32_t image = 0;
    uint32_t pc = display_list & 0x00FFFFFF;
    auto resolve = [&](uint32_t addr) { return (segments[(addr >> 24) & 0xF] + (addr & 0x00FFFFFF)) & 0x00FFFFFF; };

    for (int count = 0; count < kMaxCommands && pc + 24 <= kRdramSize; count++) {
        const uint32_t w0 = read32(rdram, pc), w1 = read32(rdram, pc + 4);
        const uint8_t op = static_cast<uint8_t>(w0 >> 24);
        const uint32_t here = pc;
        pc += 8;
        switch (op) {
            case 0xDB:  // gSPSegment
                if (((w0 >> 16) & 0xFF) == 0x06) segments[((w0 & 0xFFFF) / 4) & 0xF] = w1 & 0x00FFFFFF;
                break;
            case 0xDE:  // gSPDisplayList
                if (((w0 >> 16) & 0xFF) == 0) {
                    if (depth >= kMaxDepth) return;
                    stack[depth++] = pc;
                }
                pc = resolve(w1);
                break;
            case 0xDF:  // gSPEndDisplayList
                if (depth == 0) return;
                pc = stack[--depth];
                break;
            case 0xFD:  // SETTIMG
                image = resolve(w1);
                break;
            case 0xF5: {  // SETTILE
                Tile& tile = tiles[(w1 >> 24) & 7];
                tile.fmt = (w0 >> 21) & 7;
                tile.siz = (w0 >> 19) & 3;
                tile.cmt = (w1 >> 18) & 3;
                tile.cms = (w1 >> 8) & 3;
                break;
            }
            case 0xF2: {  // SETTILESIZE
                Tile& tile = tiles[(w1 >> 24) & 7];
                tile.uls = (w0 >> 12) & 0xFFF;
                tile.ult = w0 & 0xFFF;
                tile.lrs = (w1 >> 12) & 0xFFF;
                tile.lrt = w1 & 0xFFF;
                break;
            }
            case 0xE4: {  // TEXRECT, followed by RDPHALF_1 (s, t) and RDPHALF_2 (dsdx, dtdy)
                const uint32_t h1 = read32(rdram, here + 8), h1w1 = read32(rdram, here + 12);
                const uint32_t h2 = read32(rdram, here + 16), h2w1 = read32(rdram, here + 20);
                if ((h1 >> 24) != 0xE1 || (h2 >> 24) != 0xF1) break;
                const Tile& tile = tiles[(w1 >> 24) & 7];
                if (tile.fmt == 0 && (tile.cms & 2) && (tile.cmt & 2) && h2w1 == 0x04000400) {  // RGBA, clamped, 1:1
                    const int32_t s = static_cast<int16_t>(h1w1 >> 16), t = static_cast<int16_t>(h1w1 & 0xFFFF);
                    const auto whole = [](int32_t v) { return v >= 0 ? v & ~31 : -(-v & ~31); };  // s10.5, toward 0
                    int32_t whole_s = whole(s), whole_t = whole(t);
                    uint32_t ulx = (w1 >> 12) & 0xFFF, uly = w1 & 0xFFF, lrx = (w0 >> 12) & 0xFFF, lry = w0 & 0xFFF;
                    grow_to_tile(whole_s, ulx, lrx, tile.uls, tile.lrs);
                    grow_to_tile(whole_t, uly, lry, tile.ult, tile.lrt);
                    write32(rdram, here, (w0 & 0xFF000000) | lrx << 12 | lry);
                    write32(rdram, here + 4, (w1 & 0xFF000000) | ulx << 12 | uly);
                    write32(rdram, here + 12, (static_cast<uint32_t>(whole_s & 0xFFFF) << 16) | static_cast<uint32_t>(whole_t & 0xFFFF));
                    pc = here + 24;
                    break;
                }
                if (tile.fmt != 4 || tile.siz != 1 || !(tile.cms & 2) || !(tile.cmt & 2)) break;  // I 8-bit, clamped
                const Font* font = nullptr;
                for (int i = 0; i < font_count; i++) {
                    if (image >= fonts[i].glyphs && image < fonts[i].glyphs + kMaxGlyphs * kGlyphBytes &&
                        (image - fonts[i].glyphs) % kGlyphBytes == 0) {
                        font = &fonts[i];
                    }
                }
                if (font == nullptr) break;
                const uint32_t glyph = (image - font->glyphs) / kGlyphBytes;
                const uint32_t width = read8(rdram, font->metrics + glyph * 4);  // source width + 1

                uint32_t lrx = (w0 >> 12) & 0xFFF, lry = w0 & 0xFFF;
                uint32_t ulx = (w1 >> 12) & 0xFFF, uly = w1 & 0xFFF;
                int32_t s = static_cast<int16_t>(h1w1 >> 16), t = static_cast<int16_t>(h1w1 & 0xFFFF);
                const int32_t dsdx = static_cast<int16_t>(h2w1 >> 16), dtdy = static_cast<int16_t>(h2w1 & 0xFFFF);
                if (dsdx <= 0 || dtdy <= 0 || lrx <= ulx || lry <= uly) break;

                const uint32_t left = widen_start(s, dsdx, ulx);
                ulx -= left;
                uly = widen_top(t, dtdy, uly);
                const double right_reached = s / 32.0 + (lrx - ulx) / 4.0 * dsdx / 1024.0;
                const double bottom_reached = t / 32.0 + (lry - uly) / 4.0 * dtdy / 1024.0;
                // One column past the glyph's width when the texture has it (the soft edge of a
                // stroke in the last column lies there), else to the texture's last column.
                const double right = std::min(width + 1.0, tile.lrs / 4.0 + 1.0);
                lrx += widen_end(right_reached, right, dsdx, lrx);
                lry += widen_end(bottom_reached, tile.lrt / 4.0, dtdy, lry);

                write32(rdram, here, (w0 & 0xFF000000) | (lrx << 12) | lry);
                write32(rdram, here + 4, (w1 & 0xFF000000) | (ulx << 12) | uly);
                write32(rdram, here + 12, (static_cast<uint32_t>(s & 0xFFFF) << 16) | static_cast<uint32_t>(t & 0xFFFF));
                pc = here + 24;
                break;
            }
            default:
                break;
        }
    }
}

} // namespace conker::renderer
