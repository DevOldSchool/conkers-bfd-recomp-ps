// The crate-top B pads (as in scene 38, on the crates in the Casualty Dept.) are the top of an
// army crate split into two 32x64 CI8 textures with their own 256-colour RGBA16 palettes, flat
// resources 1052 (left) and 1053 (right): dark navy planks round a square recess, its upper edge
// lit, and in the recess a flat olive-green B with a faint grain running across it, lying on its
// side (its stem along the bottom, its smaller bowl to the right) on a near-black floor.
//
// For a PlayStation pad or the keyboard the B and its counters are taken out and the recess floor
// under them settled smoothly from the floor round them (a soft shadow, dimmer toward the bottom).
// Then the mark (a square, or an E for the keyboard, turned as the B is) is
// laid where the B was in the B's own green: the same texel where the B's face was, else the
// nearest along the same row so the grain runs on, blended into the floor at its edges. It is made
// from the player's ROM when the game starts, so no part of Rare's texture is in this repository.
#include "enhancements/pad_icons/b_pad.hpp"
#include "enhancements/pad_icons/pad_mark.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#include "hle/rt64_application.h"
#include "common/rt64_load_types.h"
#include "common/rt64_tmem_hasher.h"
#include "enhancements/sharp_menus/menu_art.hpp"

namespace conker::pad_icons {

namespace {

constexpr int kHalves[2] = {1052, 1053};  // left, right
constexpr int kW = 32, kH = 64;           // each half
constexpr int N = 64;                     // the whole crate top
constexpr int kTexelBytes = kW * kH;
constexpr int kResourceBytes = kTexelBytes + 256 * 2;
constexpr uint32_t kTlutRgba16 = 0x8000;  // G_TT_RGBA16, as RT64 hashes a TLUT draw

// The recess (inclusive), and a texel of the B's face to find it from. Then the mark, where the B
// is and as heavy as it (its counters small): its middle, and for the square and the E its outer
// half-width (the E's half height) and stroke width, in texels.
constexpr int kLeft = 12, kTop = 13, kRight = 50, kBottom = 49, kFaceX = 20, kFaceY = 24;
constexpr float kOX = 31.5f, kOY = 29.0f;
constexpr int kSquareDrop = 2;  // the square sits this many texels lower, in the middle of the recess
constexpr float kSquareOuter = 14.0f, kSquareWidth = 9.5f;
constexpr float kEOuter = 15.0f, kEWidth = 12.5f;

using Rgb = std::array<float, 3>;
using Grid = std::array<bool, N * N>;

int at(int x, int y) { return y * N + x; }
bool in(int x, int y) { return x >= 0 && y >= 0 && x < N && y < N; }
bool in_recess(int x, int y) { return x >= kLeft && x <= kRight && y >= kTop && y <= kBottom; }

// Grown by `times` texels, to all eight neighbours.
Grid dilate(Grid g, int times = 1) {
    for (int t = 0; t < times; t++) {
        const Grid prev = g;
        for (int y = 0; y < N; y++)
            for (int x = 0; x < N; x++)
                for (int j = -1; j <= 1; j++)
                    for (int i = -1; i <= 1; i++)
                        if (in(x + i, y + j) && prev[at(x + i, y + j)]) g[at(x, y)] = true;
    }
    return g;
}

// The texels four-connected to (x, y) through `g`.
Grid part_at(const Grid& g, int x, int y) {
    Grid out{};
    if (!g[at(x, y)]) return out;
    std::vector<int> members{at(x, y)};
    out[at(x, y)] = true;
    for (size_t k = 0; k < members.size(); k++) {
        const int mx = members[k] % N, my = members[k] / N;
        const int n[4][2] = {{mx - 1, my}, {mx + 1, my}, {mx, my - 1}, {mx, my + 1}};
        for (const auto& p : n) {
            if (in(p[0], p[1]) && g[at(p[0], p[1])] && !out[at(p[0], p[1])]) {
                out[at(p[0], p[1])] = true;
                members.push_back(at(p[0], p[1]));
            }
        }
    }
    return out;
}

// The mark at a texel's offset from the B's middle: the B lies with its stem along the bottom and
// its top to the right, so the E does too (a square is the same either way).
bool on_mark(const Mark& mark, float dx, float dy) {
    return mark.shape == Mark::Shape::Square ? mark.stroke(dx, dy) : mark.stroke(-dy, -dx);
}

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

std::vector<Texture> crate_pad(Mark::Shape shape) {
    const Mark mark = shape == Mark::Shape::Square ? Mark{shape, kSquareOuter, kSquareWidth} : Mark{shape, kEOuter, kEWidth};
    // The crate top as RT64 decodes it.
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

    // 1. The B: the green texels in the recess joined to its face, and its counters (what of the
    // recess the B closes round).
    Grid green{};
    for (int y = kTop; y <= kBottom; y++)
        for (int x = kLeft; x <= kRight; x++) {
            const Rgb& c = rgb[at(x, y)];
            green[at(x, y)] = c[1] > c[2] && c[1] >= c[0] && c[1] >= 24;
        }
    const Grid letter = part_at(green, kFaceX, kFaceY);
    if (!letter[at(kFaceX, kFaceY)]) return {};
    Grid open{};
    for (int i = 0; i < N * N; i++) open[i] = !letter[i];
    const Grid outside = part_at(open, 0, 0);
    Grid hole{};
    for (int i = 0; i < N * N; i++) hole[i] = !outside[i];
    hole = dilate(hole);
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) hole[at(x, y)] = hole[at(x, y)] && in_recess(x, y);

    // 2. The floor under it: a soft shadow, darker toward the bottom, so each texel the B covered
    // is settled to the mean of its four neighbours, from the floor round the B inward.
    std::vector<Rgb> out = rgb;
    for (int i = 0; i < N * N; i++)
        if (hole[i]) out[i] = {0, 0, 0};
    for (int pass = 0; pass < 2000; pass++) {
        for (int y = 0; y < N; y++)
            for (int x = 0; x < N; x++) {
                if (!hole[at(x, y)]) continue;
                Rgb sum{};
                int count = 0;
                const int n[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
                for (const auto& q : n) {
                    if (!in_recess(q[0], q[1])) continue;
                    for (int c = 0; c < 3; c++) sum[c] += out[at(q[0], q[1])][c];
                    count++;
                }
                for (int c = 0; c < 3; c++) out[at(x, y)][c] = sum[c] / float(count);
            }
    }

    // 3. The mark in the B's green: the B's own texel where its face was (clear of its edges),
    // else the nearest such along the same row, blended into the floor by how much of the texel
    // the mark covers.
    std::vector<int> core;  // the B's face away from its edges and counters
    for (int y = 1; y < N - 1; y++)
        for (int x = 1; x < N - 1; x++) {
            bool all = true;
            for (int j = -1; j <= 1; j++)
                for (int i = -1; i <= 1; i++) all = all && letter[at(x + i, y + j)];
            if (all) core.push_back(at(x, y));
        }
    if (core.empty()) return {};
    Grid is_core{};
    for (int c : core) is_core[c] = true;
    // The square sits lower than the B, and takes the B's green from as much higher.
    const int drop = shape == Mark::Shape::Square ? kSquareDrop : 0;
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            int inside = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) inside += on_mark(mark, x + (sx + 0.5f) / 4 - kOX, y + (sy + 0.5f) / 4 - kOY - float(drop));
            if (inside == 0) continue;
            const float cover = inside / 16.0f;
            const int from_y = std::max(y - drop, 0);
            int nearest = at(x, from_y);
            if (!is_core[nearest]) {
                int best = 1 << 30;
                for (int c : core) {
                    const int cx = c % N - x, cy = c / N - from_y, d = cx * cx + 16 * cy * cy;  // rows apart count most
                    if (d < best) best = d, nearest = c;
                }
            }
            Rgb& o = out[at(x, y)];
            for (int c = 0; c < 3; c++) o[c] = o[c] * (1 - cover) + rgb[nearest][c] * cover;
        }

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
