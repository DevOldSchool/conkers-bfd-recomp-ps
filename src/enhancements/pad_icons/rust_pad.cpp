// The rusty B plates (as in scene 14, Chemical Warfare) are a rusted iron plate with six rivets,
// split into two 32x64 CI8 textures with their own 256-colour RGBA16 palettes, flat resources 90
// (left) and 91 (right). A B is painted on it, worn almost to the rust: paler, pinker and more
// mottled than the plate, its counters bare rust. The plate's lower left corner is lit pale.
//
// For a PlayStation pad or the keyboard the box the B is painted in becomes bare rust: the
// plate's smooth colour (a blur) carried smoothly across it from round the box, and on that the
// rust's grain (how far its texels are off that colour) from the plate round the box. Then the
// mark (a square, or an E for the keyboard), as fat as the B, is painted where the B was in the
// B's own paint, copied in 3x3 blocks (as blotchy as Rare's) from near it on the B, its edge
// worn ragged, and its counters in shadow as the B's are. It is made from the player's ROM when the game starts, so no part of Rare's texture
// is in this repository.
#include "enhancements/pad_icons/b_pad.hpp"
#include "enhancements/pad_icons/pad_mark.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <random>

#include "hle/rt64_application.h"
#include "common/rt64_load_types.h"
#include "common/rt64_tmem_hasher.h"
#include "enhancements/sharp_menus/menu_art.hpp"

namespace conker::pad_icons {

namespace {

constexpr int kHalves[2] = {90, 91};  // left, right
constexpr int kW = 32, kH = 64;       // each half
constexpr int N = 64;                 // the whole plate
constexpr int kTexelBytes = kW * kH;
constexpr int kResourceBytes = kTexelBytes + 256 * 2;
constexpr uint32_t kTlutRgba16 = 0x8000;  // G_TT_RGBA16, as RT64 hashes a TLUT draw

// The box the B is painted in (inclusive) and the plate inside its dark rim. Then the mark, where the B is and as heavy as it: its middle, and for the square and the E
// its outer half-width (the E's half height) and stroke width, in texels.
struct Box {
    int left, top, right, bottom;
};
constexpr Box kB = {15, 6, 51, 55};
// Where the B is surely painted (its stem, its three bars, the outsides of its bowls), for its paint.
constexpr Box kStrokes[] = {{18, 9, 25, 52}, {18, 8, 42, 12}, {18, 29, 40, 35}, {18, 49, 42, 53}, {40, 12, 46, 27}, {40, 37, 47, 48}};
constexpr Box kPlate = {2, 2, 61, 61};
constexpr int kBlur = 2;  // the smooth colour's reach either way, in texels
constexpr int kGrainBlock = 3;  // the grain is copied in blocks this many texels square
constexpr float kRustGrain = 60;  // the most a texel of plain rust is off the smooth colour (r + g + b)
constexpr float kOX = 33.0f, kOY = 31.0f;
constexpr float kSquareOuter = 19.5f, kSquareWidth = 10.0f;
constexpr float kEOuter = 22.0f, kEWidth = 15.0f;
constexpr float kShade = 0.75f;  // how bright the rust in the mark's counters is, as the B's are
constexpr float kDimmest = 0.4f;  // the share of the B's paint, dimmest first, too faint to copy
constexpr int kPaintChoices = 8;  // a block's paint is one of this many of the nearest blocks of it
constexpr float kWear = 3.5f;  // how far the mark's edge wanders, at most, in texels

using Rgb = std::array<float, 3>;
using Grid = std::array<bool, N * N>;

int at(int x, int y) { return y * N + x; }
bool in_box(const Box& b, int x, int y) { return x >= b.left && x <= b.right && y >= b.top && y <= b.bottom; }
uint64_t hash(const std::vector<uint8_t>& resource) {
    std::vector<uint8_t> tmem(4096, 0);
    std::copy_n(resource.begin(), kTexelBytes, tmem.begin());
    // LoadTLUT writes each palette entry four times in the upper half of TMEM.
    for (int i = 0; i < 256; i++)
        for (int k = 0; k < 4; k++) std::copy_n(&resource[kTexelBytes + i * 2], 2, &tmem[2048 + i * 8 + k * 2]);
    RT64::LoadTile load{};
    load.fmt = 2;  // G_IM_FMT_CI
    load.siz = 1;  // G_IM_SIZ_8b
    load.line = kW / 8;
    return RT64::TMEMHasher::hash(tmem.data(), load, kW, kH, kTlutRgba16, RT64::TMEMHasher::CurrentHashVersion);
}

} // namespace

std::vector<Texture> rust_pad(Mark::Shape shape) {
    const Mark mark = shape == Mark::Shape::Square ? Mark{shape, kSquareOuter, kSquareWidth} : Mark{shape, kEOuter, kEWidth};
    // The plate as RT64 decodes it.
    std::vector<uint8_t> resources[2];
    std::vector<Rgb> rgb(N * N);
    std::vector<uint8_t> alpha(N * N);
    for (int h = 0; h < 2; h++) {
        resources[h] = menu_art::load_resource(kHalves[h]);
        const std::vector<uint8_t>& resource = resources[h];
        if (resource.size() != size_t(kResourceBytes)) return {};
        for (int y = 0; y < kH; y++)
            for (int x = 0; x < kW; x++) {
                const uint8_t index = resource[size_t(y * kW + x) ^ ((y & 1) ? 4 : 0)];  // odd rows word-swapped
                const uint16_t v = uint16_t(resource[kTexelBytes + index * 2] << 8 | resource[kTexelBytes + index * 2 + 1]);
                for (int c = 0; c < 3; c++) {
                    const int five = (v >> (11 - 5 * c)) & 31;
                    rgb[at(h * kW + x, y)][c] = float(five << 3 | five >> 2);
                }
                alpha[at(h * kW + x, y)] = (v & 1) ? 255 : 0;
            }
    }

    // 1. The plate's smooth colour (a blur), and the rust's under the B: the blur round the box
    // carried smoothly across it.
    std::vector<Rgb> smooth(N * N);
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            Rgb sum{};
            int count = 0;
            for (int j = -kBlur; j <= kBlur; j++)
                for (int i = -kBlur; i <= kBlur; i++) {
                    if (!in_box(kPlate, x + i, y + j)) continue;
                    for (int c = 0; c < 3; c++) sum[c] += rgb[at(x + i, y + j)][c];
                    count++;
                }
            for (int c = 0; c < 3; c++) smooth[at(x, y)][c] = count ? sum[c] / float(count) : rgb[at(x, y)][c];
        }
    std::vector<Rgb> rust = smooth;
    for (int pass = 0; pass < 2000; pass++)
        for (int y = kB.top; y <= kB.bottom; y++)
            for (int x = kB.left; x <= kB.right; x++) {
                Rgb& r = rust[at(x, y)];
                for (int c = 0; c < 3; c++)
                    r[c] = (rust[at(x - 1, y)][c] + rust[at(x + 1, y)][c] + rust[at(x, y - 1)][c] + rust[at(x, y + 1)][c]) / 4;
            }
    // The grain (how far each texel is off the smooth colour), for the box laid in 3x3 blocks, each
    // copied from a block picked at random among those all of `source`.
    auto grain = [&](int i, int c) { return rgb[i][c] - smooth[i][c]; };
    std::mt19937 random(3);
    auto lay_grain = [&](auto&& source) {
        std::vector<int> blocks;  // their top left texels
        for (int y = 0; y + kGrainBlock <= N; y++)
            for (int x = 0; x + kGrainBlock <= N; x++) {
                bool all = true;
                for (int j = 0; j < kGrainBlock; j++)
                    for (int i = 0; i < kGrainBlock; i++) all = all && source(x + i, y + j);
                if (all) blocks.push_back(at(x, y));
            }
        std::vector<Rgb> laid(N * N);
        if (blocks.empty()) return laid;
        for (int y = kB.top; y <= kB.bottom; y += kGrainBlock)
            for (int x = kB.left; x <= kB.right; x += kGrainBlock) {
                const int from = blocks[random() % blocks.size()];
                for (int j = 0; j < kGrainBlock; j++)
                    for (int i = 0; i < kGrainBlock; i++)
                        if (in_box(kB, x + i, y + j))
                            for (int c = 0; c < 3; c++) laid[at(x + i, y + j)][c] = grain(from + j * N + i, c);
            }
        return laid;
    };
    // The rust's: from the plate round the box, but for the rivets and the lit corner (the grain
    // there is far stronger).
    const std::vector<Rgb> rust_grain = lay_grain([&](int x, int y) {
        if (!in_box(kPlate, x, y) || in_box(kB, x, y)) return false;
        float size = 0;
        for (int c = 0; c < 3; c++) size += std::abs(grain(at(x, y), c));
        return size < kRustGrain;
    });
    std::vector<Rgb> out = rgb;
    for (int y = kB.top; y <= kB.bottom; y++)
        for (int x = kB.left; x <= kB.right; x++)
            for (int c = 0; c < 3; c++) out[at(x, y)][c] = rust[at(x, y)][c] + rust_grain[at(x, y)][c];

    // 2. The mark in the B's paint, as worn as the B: its edge wanders a texel or two either way
    // (smoothed noise), and the paint is the B's own texels, laid in 3x3 blocks each copied from a
    // block of the B's paint near it.
    auto painted = [&](int x, int y) {
        for (const Box& b : kStrokes)
            if (in_box(b, x, y)) return true;
        return false;
    };
    std::vector<int> paint_blocks;  // their top left texels
    for (int y = 0; y + kGrainBlock <= N; y++)
        for (int x = 0; x + kGrainBlock <= N; x++) {
            bool all = true;
            for (int j = 0; j < kGrainBlock; j++)
                for (int i = 0; i < kGrainBlock; i++) all = all && painted(x + i, y + j);
            if (all) paint_blocks.push_back(at(x, y));
        }
    if (paint_blocks.empty()) return {};
    // Only the brighter of them: the B is worn faintest at its lower right, where its paint is
    // hard to tell from the rust.
    {
        auto brightness = [&](int b) {
            float sum = 0;
            for (int j = 0; j < kGrainBlock; j++)
                for (int i = 0; i < kGrainBlock; i++)
                    for (int c = 0; c < 3; c++) sum += rgb[b + j * N + i][c];
            return sum;
        };
        std::vector<std::pair<float, int>> by_brightness;
        for (int b : paint_blocks) by_brightness.emplace_back(brightness(b), b);
        std::sort(by_brightness.begin(), by_brightness.end());
        paint_blocks.clear();
        for (size_t k = size_t(by_brightness.size() * kDimmest); k < by_brightness.size(); k++) paint_blocks.push_back(by_brightness[k].second);
    }
    std::vector<Rgb> paint(N * N);
    for (int y = kB.top; y <= kB.bottom; y += kGrainBlock)
        for (int x = kB.left; x <= kB.right; x += kGrainBlock) {
            // One of the blocks nearest (the plate is lit unevenly, paler toward its lower left).
            std::vector<std::pair<int, int>> near;
            for (int b : paint_blocks) {
                const int bx = b % N - x, by = b / N - y;
                near.emplace_back(bx * bx + by * by, b);
            }
            const size_t pick = std::min<size_t>(kPaintChoices, near.size());
            std::partial_sort(near.begin(), near.begin() + pick, near.end());
            const int from = near[random() % pick].second;
            for (int j = 0; j < kGrainBlock; j++)
                for (int i = 0; i < kGrainBlock; i++)
                    if (in_box(kB, x + i, y + j)) paint[at(x + i, y + j)] = rgb[from + j * N + i];
        }
    std::array<float, N * N> wear{};
    {
        std::array<float, N * N> noise;
        for (float& n : noise) n = float(random() % 2001) / 1000.0f - 1;
        for (int y = 0; y < N; y++)
            for (int x = 0; x < N; x++) {
                float sum = 0;
                int count = 0;
                for (int j = -1; j <= 1; j++)
                    for (int i = -1; i <= 1; i++)
                        if (in_box(kPlate, x + i, y + j)) sum += noise[at(x + i, y + j)], count++;
                wear[at(x, y)] = count ? sum / float(count) * kWear : 0;
            }
    }
    for (int y = kB.top; y <= kB.bottom; y++)
        for (int x = kB.left; x <= kB.right; x++) {
            // The mark's counter (the E's gaps), in shadow as the B's counters are.
            const float dx = x + 0.5f - kOX, dy = y + 0.5f - kOY;
            if (mark.within(dx, dy))
                for (float& c : out[at(x, y)]) c *= kShade;
            const float d = mark.distance(dx, dy) + wear[at(x, y)];
            const float cover = std::clamp(0.5f - d, 0.0f, 1.0f);
            if (cover <= 0) continue;
            Rgb& o = out[at(x, y)];
            for (int c = 0; c < 3; c++) o[c] = o[c] * (1 - cover) + paint[at(x, y)][c] * cover;
        }
    for (Rgb& o : out)
        for (float& c : o) c = std::clamp(c, 0.0f, 255.0f);

    std::vector<Texture> textures;
    for (int h = 0; h < 2; h++) {
        Texture t{hash(resources[h]), kW, kH, std::vector<uint8_t>(kW * kH * 4)};
        for (int y = 0; y < kH; y++)
            for (int x = 0; x < kW; x++) {
                const int i = at(h * kW + x, y);
                uint8_t* p = &t.rgba[(y * kW + x) * 4];
                for (int c = 0; c < 3; c++) p[c] = uint8_t(std::lround(out[i][c]));
                p[3] = alpha[i];
            }
        textures.push_back(std::move(t));
    }
    return textures;
}

} // namespace conker::pad_icons
