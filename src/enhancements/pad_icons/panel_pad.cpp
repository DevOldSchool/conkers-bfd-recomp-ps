// The dark stone panel B pads (as in scene 54) are a 64x64 CI4 texture with a 16-colour RGBA16
// palette, flat resource 3199: a mottled blue-grey slab, a dark frame cut round it, and inside the
// frame a raised B (x 20-45, y 12-47) picked out by a dark edge along its top and left and lit
// patches on its lower and right sides.
//
// For a PlayStation pad or the keyboard the B is taken out and the slab filled from its edges
// inward, each step copying the 3x3 patch of the slab inside the frame, clear of the B, that best
// matches what is already round it and lies at about the same height (the slab is brighter toward
// the bottom). Then the mark (a square or an E) is raised where the B was in the same way: its
// face the slab a shade darker, its edges facing up and left in the B's dark shades, those facing down and right
// lit. It is made from the player's ROM when the game starts, so no part of Rare's texture is in
// this repository.
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

constexpr int kResource = 3199;
constexpr int N = 64;
constexpr int kTexelBytes = N * N / 2;
constexpr uint32_t kTlutRgba16 = 0x8000;  // G_TT_RGBA16, as RT64 hashes a TLUT draw

// The B with its edges (inclusive), and the slab inside the frame the fill copies from.
constexpr int kLeft = 17, kTop = 12, kRight = 46, kBottom = 48;
bool in_box(int x, int y) { return x >= kLeft && x <= kRight && y >= kTop && y <= kBottom; }
bool on_slab(int x, int y) { return x >= 9 && x <= 53 && y >= 9 && y <= 52; }
// The mark: its middle, its outer half-width (the E's half height) and stroke, and how wide its
// edges are, in texels.
constexpr float kOX = 32.0f, kOY = 30.0f, kOuter = 16.0f, kWidth = 7.5f, kEdge = 1.8f;
constexpr float kFace = 0.75f;  // how bright the mark's face is, of the slab under it, as the B's is
// The B's shades: its dark edge, darkest first, and its lit one, brightest first.
constexpr int kDark[] = {9, 8, 7};
constexpr int kLit[] = {11, 10, 4};

using Grid = std::array<bool, N * N>;
using Rgb = std::array<float, 3>;

int at(int x, int y) { return y * N + x; }
bool in(int x, int y) { return x >= 0 && y >= 0 && x < N && y < N; }

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

uint64_t hash(const std::vector<uint8_t>& resource) {
    std::vector<uint8_t> tmem(4096, 0);
    std::copy_n(resource.begin(), kTexelBytes, tmem.begin());
    // LoadTLUT writes each palette entry four times in the upper half of TMEM.
    for (int i = 0; i < 16; i++)
        for (int k = 0; k < 4; k++) std::copy_n(&resource[kTexelBytes + i * 2], 2, &tmem[2048 + i * 8 + k * 2]);
    RT64::LoadTile load{};
    load.fmt = 2;  // G_IM_FMT_CI
    load.siz = 0;  // G_IM_SIZ_4b
    load.line = N / 2 / 8;
    return RT64::TMEMHasher::hash(tmem.data(), load, N, N, kTlutRgba16, RT64::TMEMHasher::CurrentHashVersion);
}

} // namespace

std::optional<Texture> panel_pad(Mark::Shape shape) {
    const std::vector<uint8_t> resource = menu_art::load_resource(kResource);
    if (resource.size() != size_t(kTexelBytes + 32)) return std::nullopt;

    // The palette as RT64 decodes RGBA16, and each texel's entry (odd rows are word-swapped).
    std::array<Rgb, 16> palette;
    std::array<bool, 16> opaque;
    for (int i = 0; i < 16; i++) {
        const uint16_t v = uint16_t(resource[kTexelBytes + i * 2] << 8 | resource[kTexelBytes + i * 2 + 1]);
        for (int c = 0; c < 3; c++) {
            const int five = (v >> (11 - 5 * c)) & 31;
            palette[i][c] = float(five << 3 | five >> 2);
        }
        opaque[i] = v & 1;
    }
    std::array<int, N * N> index;
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            const int byte = (y * N + x) / 2 ^ ((y & 1) ? 4 : 0);
            index[at(x, y)] = (x & 1) ? resource[byte] & 15 : resource[byte] >> 4;
        }

    // 1. The B taken out and filled from its edges inward, each step copying the 3x3 patch of the
    // slab (clear of the B) that best matches the known texels round it at about the same height.
    Grid hole{};
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) hole[at(x, y)] = in_box(x, y);
    const Grid away = dilate(hole, 2);
    std::vector<int> patches;  // their middles
    for (int y = 1; y < N - 1; y++)
        for (int x = 1; x < N - 1; x++) {
            bool all = true;
            for (int j = -1; j <= 1; j++)
                for (int i = -1; i <= 1; i++) all = all && on_slab(x + i, y + j) && !away[at(x + i, y + j)];
            if (all) patches.push_back(at(x, y));
        }
    if (patches.size() < 2) return std::nullopt;
    constexpr float kHeight = 120;  // how much a row apart counts against a patch
    std::mt19937 random(3);
    std::array<int, N * N> out = index;
    Grid known;
    for (int i = 0; i < N * N; i++) known[i] = !hole[i];
    auto distance = [&](int a, int b) {
        float d = 0;
        for (int c = 0; c < 3; c++) d += (palette[a][c] - palette[b][c]) * (palette[a][c] - palette[b][c]);
        return d;
    };
    for (;;) {
        // The texel to fill beside known texels with the most known texels round it.
        int best_i = -1, best_known = -1;
        for (int y = 0; y < N; y++)
            for (int x = 0; x < N; x++) {
                if (known[at(x, y)]) continue;
                bool front = false;
                const int n[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
                for (const auto& p : n) front = front || (in(p[0], p[1]) && known[at(p[0], p[1])]);
                if (!front) continue;
                int count = 0;
                for (int j = -1; j <= 1; j++)
                    for (int i = -1; i <= 1; i++) count += in(x + i, y + j) && known[at(x + i, y + j)];
                if (count > best_known) best_known = count, best_i = at(x, y);
            }
        if (best_i < 0) break;
        const int x = best_i % N, y = best_i / N;
        std::vector<std::pair<float, int>> scores;
        for (int p : patches) {
            const int dy = p / N - y;
            float d = kHeight * float(dy * dy);
            for (int j = -1; j <= 1; j++)
                for (int i = -1; i <= 1; i++)
                    if (in(x + i, y + j) && known[at(x + i, y + j)]) d += distance(out[at(x + i, y + j)], index[p + j * N + i]);
            scores.emplace_back(d, p);
        }
        std::partial_sort(scores.begin(), scores.begin() + 2, scores.end());
        const int chosen = scores[random() % 2].second;
        for (int j = -1; j <= 1; j++)
            for (int i = -1; i <= 1; i++) {
                const int t = at(x + i, y + j);
                if (!in(x + i, y + j) || known[t]) continue;
                out[t] = index[chosen + j * N + i];
                known[t] = true;
            }
    }

    // 2. The mark raised as the B is: along its edges, from 4x4 samples, how much of each texel is
    // edge and which way that edge faces; up and left in the dark shades, down and right lit, the
    // face between them the slab a shade darker.
    std::array<int, 16> darker;
    for (int i = 0; i < 16; i++) {
        float best = 1e30f;
        darker[i] = i;
        for (int j = 0; j < 16; j++) {
            float d = 0;
            for (int c = 0; c < 3; c++) d += (palette[j][c] - palette[i][c] * kFace) * (palette[j][c] - palette[i][c] * kFace);
            if (opaque[j] && d < best) best = d, darker[i] = j;
        }
    }
    const Mark mark{shape, kOuter, kWidth};
    std::mt19937 shade_random(5);
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            if (mark.distance(x + 0.5f - kOX, y + 0.5f - kOY) <= 0) out[at(x, y)] = darker[out[at(x, y)]];
            int edge = 0;
            float toward_light = 0;  // down and right
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    const float dx = x + (sx + 0.5f) / 4 - kOX, dy = y + (sy + 0.5f) / 4 - kOY;
                    const float d = mark.distance(dx, dy);
                    if (d > 0 || d < -kEdge) continue;
                    const square_symbol::Facing f = mark.out(dx, dy);
                    edge++;
                    toward_light += (f.x + f.y) * float(M_SQRT1_2);
                }
            if (edge < 6) continue;
            toward_light /= float(edge);
            const int k = int(shade_random() % 3);
            if (toward_light < -0.3f) out[at(x, y)] = kDark[std::min(k, 2 - int(edge >= 12))];
            else if (toward_light > 0.3f) out[at(x, y)] = kLit[std::min(k, 2 - int(edge >= 12))];
        }

    Texture texture{hash(resource), N, N, std::vector<uint8_t>(N * N * 4)};
    for (int i = 0; i < N * N; i++) {
        for (int c = 0; c < 3; c++) texture.rgba[i * 4 + c] = uint8_t(palette[out[i]][c]);
        texture.rgba[i * 4 + 3] = opaque[out[i]] ? 255 : 0;
    }
    return texture;
}

} // namespace conker::pad_icons
