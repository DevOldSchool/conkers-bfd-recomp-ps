// The game's text font is 95 grayscale RLE glyphs at ROM 0x40F10..0x42450. At boot func_15015920
// walks them and func_15015A38 decodes each into its own I8 buffer: a zero row above and below the
// glyph, a zero column at its left, rows padded to a multiple of 8 bytes and odd rows word-swapped
// for TMEM. A glyph is drawn by loading its buffer to TMEM 0 as it is, so each glyph is one texture
// with a stable RT64 hash. Upscaled by RT64, their soft one-texel edges blur.
//
// This makes each glyph eight times larger with Super-xBR (Hyllian's edge-directed 2x pixel-art
// upscaler, applied three times), which keeps Rare's pixels and soft edges but is smoother and less
// blurred than the N64's three-point filtering, and hands the results to RT64 as a texture pack in
// memory, keyed by the hash RT64 computes for each glyph (TMEMHasher). The game draws each glyph a
// little short of its edges; src/platform/font_rects.cpp widens those rectangles so the sharper
// glyphs are not cut off.
//
// CONKER_DUMP_SHARP_FONT=<dir> writes the pack's files to <dir>, to edit by hand and load from
// conker-recomp-data/texture_packs; tools/font_pack.py writes the same pack outside the game. Packs in
// conker-recomp-data/texture_packs load after this one and win where they replace the same glyphs.
// CONKER_SHARP_FONT=0 leaves the font as it is.
#include "enhancements/sharp_font/sharp_font.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "hle/rt64_application.h"
#include "common/rt64_filesystem.h"
#include "common/rt64_load_types.h"
#include "common/rt64_tmem_hasher.h"
#include "platform/memory_pack.hpp"

namespace conker::sharp_font {

namespace {

constexpr uint32_t kFontStart = 0x40F10;
constexpr uint32_t kFontEnd = 0x42450;
constexpr int kScale = 8;  // RT64 loads PNGs without mipmaps; 8 suits pictures drawn at 5x to 10x
constexpr int kPad = 2;    // empty pixels around a glyph, as far as Super-xBR's 4x4 neighbourhood reaches
constexpr int kFull = 240; // the font's brightest intensity

struct Glyph {
    int width = 0;   // source width + 1: the columns the game advances over
    int stride = 0;  // row length in the glyph's buffer
    int rows = 0;    // rows sampled: the zero row above and the glyph's own
    std::vector<uint8_t> image;  // stride x rows intensities, without the TMEM word swap
};

// Decodes every record as func_15015A38 does.
std::vector<Glyph> decode(std::span<const uint8_t> rom) {
    std::vector<Glyph> glyphs;
    if (rom.size() < kFontEnd) {
        return glyphs;
    }
    uint32_t cursor = kFontStart;
    while (cursor + 0xF < kFontEnd) {
        const uint8_t* record = rom.data() + cursor;
        const uint32_t size = (uint32_t(record[4]) << 24) | (uint32_t(record[5]) << 16) | (uint32_t(record[6]) << 8) | record[7];
        Glyph glyph;
        glyph.width = record[0] + 1;
        const int height = record[1] + 1;
        glyph.stride = (glyph.width + 7) & ~7;
        glyph.rows = height + 1;
        glyph.image.assign(size_t(glyph.stride) * glyph.rows, 0);
        uint32_t offset = 8;
        for (int y = 1; y <= height; y++) {
            for (int x = 1; x < glyph.width;) {
                if (cursor + offset >= kFontEnd) {
                    return {};
                }
                const uint8_t packed = record[offset++];
                const int run = (packed & 0xF) + 1;
                for (int i = 0; i < run && x + i < glyph.stride; i++) {
                    glyph.image[size_t(y) * glyph.stride + x + i] = packed & 0xF0;
                }
                x += run;
            }
        }
        glyphs.push_back(std::move(glyph));
        if (size == 0) {
            return {};
        }
        cursor += size;
    }
    return glyphs;
}

// RT64's hash for a glyph's draw: its buffer in TMEM 0, I 8-bit, stride x rows texels.
uint64_t hash(const Glyph& glyph) {
    std::vector<uint8_t> tmem(4096, 0);
    for (int y = 0; y < glyph.rows; y++) {
        const int swap = (y & 1) ? 4 : 0;
        for (int x = 0; x < glyph.stride; x++) {
            tmem[size_t(y) * glyph.stride + x] = glyph.image[size_t(y) * glyph.stride + (x ^ swap)];
        }
    }
    RT64::LoadTile tile{};
    tile.fmt = 4;  // G_IM_FMT_I
    tile.siz = 1;  // G_IM_SIZ_8b
    tile.line = uint16_t(glyph.stride / 8);
    return RT64::TMEMHasher::hash(tmem.data(), tile, uint16_t(glyph.stride), uint16_t(glyph.rows), 0,
                                  RT64::TMEMHasher::CurrentHashVersion);
}

// --- Super-xBR -----------------------------------------------------------------------------------

using Image = std::vector<std::vector<double>>;  // [y][x]
using Block = double[4][4];                       // [x][y]

constexpr double kW1 = -0.129633, kW2 = 0.129633 + 0.5;
constexpr double kW3 = -0.175068, kW4 = 0.175068 + 0.5;
constexpr double kEdgeWeights[6] = {2.0, 1.0, -1.0, 4.0, -1.0, 1.0};
constexpr double kRotatedWeights[6] = {2.0, 0.0, 0.0, 0.0, 0.0, 0.0};

// How much stronger the edge across one diagonal of the neighbourhood is than across the other.
double diagonal_edge(const Block& m, const double* wp) {
    auto df = [](double a, double b) { return std::fabs(a - b); };
    const double dw1 = wp[0] * (df(m[0][2], m[1][1]) + df(m[1][1], m[2][0]) + df(m[1][3], m[2][2]) + df(m[2][2], m[3][1])) +
                      wp[1] * (df(m[0][3], m[1][2]) + df(m[2][1], m[3][0])) +
                      wp[2] * (df(m[0][3], m[2][1]) + df(m[1][2], m[3][0])) + wp[3] * df(m[1][2], m[2][1]) +
                      wp[4] * (df(m[0][2], m[2][0]) + df(m[1][3], m[3][1])) +
                      wp[5] * (df(m[0][1], m[1][0]) + df(m[2][3], m[3][2]));
    const double dw2 = wp[0] * (df(m[0][1], m[1][2]) + df(m[1][2], m[2][3]) + df(m[1][0], m[2][1]) + df(m[2][1], m[3][2])) +
                      wp[1] * (df(m[0][0], m[1][1]) + df(m[2][2], m[3][3])) +
                      wp[2] * (df(m[0][0], m[2][2]) + df(m[1][1], m[3][3])) + wp[3] * df(m[1][1], m[2][2]) +
                      wp[4] * (df(m[1][0], m[3][2]) + df(m[0][1], m[2][3])) +
                      wp[5] * (df(m[0][2], m[1][3]) + df(m[2][0], m[3][1]));
    return dw1 - dw2;
}

// The centre of the neighbourhood, interpolated along the diagonal with the weaker edge and clamped
// to the four pixels around it, so edges do not ring.
double interpolate(const Block& m, const double* wp, double wa, double wb) {
    const double lo = std::min({m[1][1], m[2][1], m[1][2], m[2][2]});
    const double hi = std::max({m[1][1], m[2][1], m[1][2], m[2][2]});
    const double value = diagonal_edge(m, wp) <= 0 ? wa * (m[0][3] + m[3][0]) + wb * (m[1][2] + m[2][1])
                                                  : wa * (m[0][0] + m[3][3]) + wb * (m[1][1] + m[2][2]);
    return std::clamp(value, lo, hi);
}

Image double_size(const Image& img) {
    const int h = int(img.size()), w = int(img[0].size());
    const int oh = h * 2, ow = w * 2;
    Image out(oh, std::vector<double>(ow, 0.0));
    auto src = [&](int x, int y) { return img[std::clamp(y, 0, h - 1)][std::clamp(x, 0, w - 1)]; };
    auto dst = [&](int x, int y) { return out[std::clamp(y, 0, oh - 1)][std::clamp(x, 0, ow - 1)]; };
    Block m;
    // Each original pixel, and the pixel between it and its three neighbours below right.
    for (int y = 0; y < oh; y += 2) {
        for (int x = 0; x < ow; x += 2) {
            const int cx = x / 2, cy = y / 2;
            for (int sx = 0; sx < 4; sx++)
                for (int sy = 0; sy < 4; sy++) m[sx][sy] = src(cx + sx - 1, cy + sy - 1);
            out[y][x] = out[y][x + 1] = out[y + 1][x] = img[cy][cx];
            out[y + 1][x + 1] = interpolate(m, kEdgeWeights, kW1, kW2);
        }
    }
    // The other two pixels of each block, from neighbourhoods rotated by 45 degrees.
    for (int y = 0; y < oh; y += 2) {
        for (int x = 0; x < ow; x += 2) {
            for (int sx = 0; sx < 4; sx++)
                for (int sy = 0; sy < 4; sy++) m[sx][sy] = dst(sx + sy - 2 + x, sx - sy + y);
            const double right = interpolate(m, kRotatedWeights, kW3, kW4);
            for (int sx = 0; sx < 4; sx++)
                for (int sy = 0; sy < 4; sy++) m[sx][sy] = dst(sx + sy - 3 + x, sx - sy + 1 + y);
            const double below = interpolate(m, kRotatedWeights, kW3, kW4);
            out[y][x + 1] = right;
            out[y + 1][x] = below;
        }
    }
    // Refine every pixel, from the bottom right.
    for (int y = oh - 1; y >= 0; y--) {
        for (int x = ow - 1; x >= 0; x--) {
            for (int sx = 0; sx < 4; sx++)
                for (int sy = 0; sy < 4; sy++) m[sx][sy] = dst(x + sx - 2, y + sy - 2);
            out[y][x] = interpolate(m, kEdgeWeights, kW1, kW2);
        }
    }
    return out;
}

// The glyph kScale times larger as RGBA, white with its intensity as alpha (R = G = B = A) as RT64
// decodes the original I8 texture. The glyph is enlarged with empty pixels around it, so a stroke in
// its last column ends with its own falloff inside that column instead of running into the
// texture's edge. Super-xBR keeps each pixel's value at its block's top left, where the N64 samples
// it, so the pack draws it without RT64's half-texel shift.
std::vector<uint8_t> enlarge(const Glyph& glyph, int& out_width, int& out_height) {
    Image img(glyph.rows + 2 * kPad, std::vector<double>(glyph.stride + 2 * kPad, 0.0));
    for (int y = 0; y < glyph.rows; y++)
        for (int x = 0; x < glyph.stride; x++) img[y + kPad][x + kPad] = glyph.image[size_t(y) * glyph.stride + x];
    for (int factor = 1; factor < kScale; factor *= 2) {
        img = double_size(img);
    }
    out_width = glyph.stride * kScale;
    out_height = glyph.rows * kScale;
    const int edge = kPad * kScale;
    std::vector<uint8_t> rgba(size_t(out_width) * out_height * 4);
    for (int y = 0; y < out_height; y++) {
        for (int x = 0; x < out_width; x++) {
            const double c = std::nearbyint(img[y + edge][x + edge]);  // ties to even, as Python's round
            const uint8_t v = uint8_t(std::clamp(c, 0.0, 255.0));
            uint8_t* px = &rgba[(size_t(y) * out_width + x) * 4];
            px[0] = px[1] = px[2] = px[3] = v;
        }
    }
    return rgba;
}

} // namespace

bool enabled() {
    const char* value = std::getenv("CONKER_SHARP_FONT");
    return value == nullptr || std::string(value) != "0";
}

void dump_pack(const RT64::FileSystem& pack) {
    const char* dir = std::getenv("CONKER_DUMP_SHARP_FONT");
    if (dir == nullptr || dir[0] == '\0') {
        return;
    }
    memory_pack::write_files(pack, dir);
    std::printf("[conker] sharp font written to %s\n", dir);
}

std::shared_ptr<RT64::FileSystem> make_pack(std::span<const uint8_t> rom) {
    const std::vector<Glyph> glyphs = decode(rom);
    if (glyphs.size() != 95) {
        std::fprintf(stderr, "[conker] sharp font: %zu glyphs in the ROM's font, expected 95; left as it is\n", glyphs.size());
        return nullptr;
    }
    auto pack = std::make_shared<memory_pack::Pack>();
    for (size_t i = 0; i < glyphs.size(); i++) {
        int width = 0, height = 0;
        const std::vector<uint8_t> rgba = enlarge(glyphs[i], width, height);
        char name[32];
        std::snprintf(name, sizeof(name), "font/glyph_%02zu.png", i);
        pack->add(name, hash(glyphs[i]), width, height, rgba.data());
    }
    pack->finish();
    return pack;
}

} // namespace conker::sharp_font
