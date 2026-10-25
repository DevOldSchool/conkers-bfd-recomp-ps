// The teal B pads set in a pale green floor (as in scene 10) are part of a floor texture split into
// two 32x64 CI8 halves with their own 256-colour RGBA16 palettes, flat resources 1151 (left) and
// 1152 (right): a teal ring with a dark middle at the top of the tile (its top edge is on the tile
// above), and in it a flat teal B with a slightly lighter edge. The floor is drawn stretched about
// 1.41 times along the tile's height, so the ring, 31 texels wide and 22 high, is round on the
// floor.
//
// For a PlayStation pad the B is taken out and the dark under it filled from its edges inward, each
// step copying the 3x3 patch of the ring's own dark middle (clear of the B) that best matches what
// is already round it. Then a square, as wide on the floor as it is high, stands where the B was: its
// edge the B's edge texels in order round it, the rest the B's own teal. It is made from the
// player's ROM when the game starts, so no part of Rare's texture is in this repository.
#include "enhancements/pad_icons/b_pad.hpp"
#include "enhancements/pad_icons/square_symbol.hpp"

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

constexpr int kHalves[2] = {1151, 1152};  // left, right
constexpr int kW = 32, kH = 64;           // each half
constexpr int N = 64;                     // the whole tile
constexpr int kTexelBytes = kW * kH;
constexpr int kResourceBytes = kTexelBytes + 256 * 2;
constexpr uint32_t kTlutRgba16 = 0x8000;  // G_TT_RGBA16, as RT64 hashes a TLUT draw

// A texel of the ring and one of the B, the ring's middle (and the square's), how much longer the
// floor draws the tile's height than its width, and the square's half height and stroke (a share of
// its half height).
constexpr int kRingX = 19, kRingY = 7, kLetterX = 30, kLetterY = 7;
constexpr float kOX = 32.5f, kOY = 7.0f, kStretch = 1.41f;
constexpr float kOuterY = 5.5f, kStroke = 0.45f;

using Rgb = std::array<float, 3>;
using Grid = std::array<bool, N * N>;

int at(int x, int y) { return y * N + x; }
bool in(int x, int y) { return x >= 0 && y >= 0 && x < N && y < N; }
float lum(const Rgb& c) { return (c[0] + c[1] + c[2]) / 3; }

// The four-neighbour dilation and erosion scipy.ndimage uses by default.
Grid dilate(Grid g) {
    const Grid prev = g;
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            bool v = prev[at(x, y)];
            const int n[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
            for (const auto& p : n) v = v || (in(p[0], p[1]) && prev[at(p[0], p[1])]);
            g[at(x, y)] = v;
        }
    return g;
}

Grid erode(const Grid& g) {
    Grid inverse;
    for (size_t i = 0; i < g.size(); i++) inverse[i] = !g[i];
    const Grid grown = dilate(inverse);
    Grid out;
    for (size_t i = 0; i < g.size(); i++) out[i] = !grown[i];
    return out;
}

// The part of `g` four-connected to (x, y).
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

struct Half {
    std::vector<uint8_t> resource;
    std::array<Rgb, 256> palette;
    std::array<bool, 256> opaque;
};

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

std::vector<Texture> oval_pad() {
    // The tile as RT64 decodes it.
    Half halves[2];
    std::vector<Rgb> rgb(N * N);
    std::vector<uint8_t> alpha(N * N);
    for (int h = 0; h < 2; h++) {
        Half& half = halves[h];
        half.resource = menu_art::load_resource(kHalves[h]);
        if (half.resource.size() != size_t(kResourceBytes)) return {};
        for (int i = 0; i < 256; i++) {
            const uint16_t v = uint16_t(half.resource[kTexelBytes + i * 2] << 8 | half.resource[kTexelBytes + i * 2 + 1]);
            for (int c = 0; c < 3; c++) {
                const int five = (v >> (11 - 5 * c)) & 31;
                half.palette[i][c] = float(five << 3 | five >> 2);
            }
            half.opaque[i] = v & 1;
        }
        for (int y = 0; y < kH; y++)
            for (int x = 0; x < kW; x++) {
                const uint8_t index = half.resource[size_t(y * kW + x) ^ ((y & 1) ? 4 : 0)];  // odd rows word-swapped
                rgb[at(h * kW + x, y)] = half.palette[index];
                alpha[at(h * kW + x, y)] = half.opaque[index] ? 255 : 0;
            }
    }

    // 1. The B: the teal inside the ring, apart from the ring; and the dark middle round it.
    Grid teal{}, dark_or_b{};
    for (int i = 0; i < N * N; i++) {
        teal[i] = rgb[i][1] - rgb[i][0] > 30 && lum(rgb[i]) > 48;
        dark_or_b[i] = !teal[i] && lum(rgb[i]) < 50;
    }
    const Grid ring = part_at(teal, kRingX, kRingY);
    const Grid letter = part_at(teal, kLetterX, kLetterY);
    if (ring[at(kLetterX, kLetterY)] || !letter[at(kLetterX, kLetterY)]) return {};
    for (int i = 0; i < N * N; i++) dark_or_b[i] = dark_or_b[i] || letter[i];
    const Grid middle = part_at(dark_or_b, kLetterX, kLetterY);
    Grid hole = dilate(letter);
    for (int i = 0; i < N * N; i++) hole[i] = letter[i] || (hole[i] && middle[i]);

    // 2. The dark under it, from the edges inward, each step copying the 3x3 patch of the middle
    // (clear of the B) that best matches what is already round it.
    constexpr int kHalf = 1;
    std::vector<int> patches;  // their middles
    for (int y = kHalf; y < N - kHalf; y++)
        for (int x = kHalf; x < N - kHalf; x++) {
            bool clear = true;
            for (int j = -kHalf; j <= kHalf && clear; j++)
                for (int i = -kHalf; i <= kHalf && clear; i++) clear = middle[at(x + i, y + j)] && !hole[at(x + i, y + j)];
            if (clear) patches.push_back(at(x, y));
        }
    if (patches.empty()) return {};
    std::mt19937 random(3);
    auto mirror = [](int v) { return v < 0 ? -v - 1 : v >= N ? 2 * N - v - 1 : v; };
    std::vector<Rgb> out = rgb;
    for (;;) {
        // The hole texel beside known texels with the most known round it.
        int best_i = -1;
        float best_known = -1;
        for (int y = 0; y < N; y++)
            for (int x = 0; x < N; x++) {
                if (!hole[at(x, y)]) continue;
                bool front = false;
                const int n[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
                for (const auto& p : n) front = front || (in(p[0], p[1]) && !hole[at(p[0], p[1])]);
                if (!front) continue;
                float known = 0;
                for (int j = -kHalf; j <= kHalf; j++)
                    for (int i = -kHalf; i <= kHalf; i++) known += !hole[at(mirror(x + i), mirror(y + j))];
                if (known > best_known) best_known = known, best_i = at(x, y);
            }
        if (best_i < 0) break;
        const int x = best_i % N, y = best_i / N;
        std::vector<std::pair<float, int>> scores;
        for (int p : patches) {
            float d = 0;
            for (int j = -kHalf; j <= kHalf; j++)
                for (int i = -kHalf; i <= kHalf; i++) {
                    if (!in(x + i, y + j) || hole[at(x + i, y + j)]) continue;
                    const Rgb& a = out[at(x + i, y + j)];
                    const Rgb& b = rgb[p + j * N + i];
                    for (int c = 0; c < 3; c++) d += (a[c] - b[c]) * (a[c] - b[c]);
                }
            scores.emplace_back(d, p);
        }
        std::partial_sort(scores.begin(), scores.begin() + std::min<size_t>(3, scores.size()), scores.end());
        const int chosen = scores[random() % std::min<size_t>(3, scores.size())].second;
        for (int j = -kHalf; j <= kHalf; j++)
            for (int i = -kHalf; i <= kHalf; i++) {
                if (!in(x + i, y + j) || !hole[at(x + i, y + j)]) continue;
                out[at(x + i, y + j)] = rgb[chosen + j * N + i];
                hole[at(x + i, y + j)] = false;
            }
    }

    // 3. The square, its sides equal on the floor: its edge texels the B's, in order round it (by the angle on
    // the floor), the rest the B's teal, the same texel where the B was, else the nearest.
    const float outer_x = kOuterY * kStretch;
    Grid o{};
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            int inside = 0;
            for (int sy = 0; sy < 8; sy++)
                for (int sx = 0; sx < 8; sx++) {
                    const float d = square_symbol::reach((x + (sx + 0.5f) / 8 - kOX) / outer_x, (y + (sy + 0.5f) / 8 - kOY) / kOuterY);
                    inside += d >= 1 - kStroke && d <= 1;
                }
            o[at(x, y)] = inside > 64 * 0.45f;
        }
    auto angle = [](int x, int y) { return std::atan2((y + 0.5f - kOY) * kStretch, x + 0.5f - kOX); };
    const Grid o_inside = erode(o), core_mask = erode(letter);
    std::vector<std::pair<float, int>> edge;  // the B's edge, by angle
    std::vector<int> core;
    for (int i = 0; i < N * N; i++) {
        if (letter[i] && !core_mask[i]) edge.emplace_back(angle(i % N, i / N), i);
        if (core_mask[i]) core.push_back(i);
    }
    if (edge.empty() || core.empty()) return {};
    std::sort(edge.begin(), edge.end());
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            const int i = at(x, y);
            if (!o[i]) continue;
            if (!o_inside[i]) {
                const float turn = (angle(x, y) + float(M_PI)) / (2 * float(M_PI));
                out[i] = rgb[edge[size_t(turn * edge.size()) % edge.size()].second];
            } else if (core_mask[i]) {
                out[i] = rgb[i];
            } else {
                int nearest = core[0];
                for (int c : core) {
                    const int cx = c % N, cy = c / N, nx = nearest % N, ny = nearest / N;
                    if ((cx - x) * (cx - x) + (cy - y) * (cy - y) < (nx - x) * (nx - x) + (ny - y) * (ny - y)) nearest = c;
                }
                out[i] = rgb[nearest];
            }
        }

    std::vector<Texture> textures;
    for (int h = 0; h < 2; h++) {
        Texture t{hash(halves[h].resource), kW, kH, std::vector<uint8_t>(kW * kH * 4)};
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
