// The rug B pad (as in scene 60, Count Batula's house) is a 64x64 CI4 texture with a 16-colour
// RGBA16 palette, flat resource 2847: a pale brass plaque in a dark wooden frame, a curly ink
// flourish either side of its lower half, and in the middle a calligraphic B in dark brown ink, its
// top stroke swept out to the left into a curl.
//
// For a PlayStation pad the B is taken out (its dark ink texels joined to its stem, and the brown
// ones round them) and filled from its edges inward, each step copying the 3x3 patch of the
// plaque, clear of the B and of any ink, that best matches what is already round it and lies at
// about the same height (the plaque is darker and more mottled toward the top). Then an O is
// written where the B was, as with a broad nib: thick down its sides, thin over its top and bottom,
// in the B's own inks, blended into the plaque at its edges. It is made from the player's ROM when
// the game starts, so no part of Rare's texture is in this repository.
#include "enhancements/pad_icons/b_pad.hpp"

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

constexpr int kResource = 2847;
constexpr int N = 64;
constexpr int kTexelBytes = N * N / 2;
constexpr uint32_t kTlutRgba16 = 0x8000;  // G_TT_RGBA16, as RT64 hashes a TLUT draw

// The B lies in x 9-46, y 20-47 (inclusive), but for the plaque's notch above its curl (x < 17,
// y < 24); its stem is at (27, 32). The plaque the fill copies from: x 8-55, y 7-56.
constexpr int kLeft = 9, kTop = 20, kRight = 46, kBottom = 47, kStemX = 27, kStemY = 32;
// The O: its middle, its half width and height (to the middle of the stroke), how much it leans
// (degrees, clockwise), and the nib: its length and angle (degrees, clockwise from the right) and
// its thickness, in texels.
constexpr float kOX = 32.0f, kOY = 31.0f, kRadiusX = 9.5f, kRadiusY = 12.5f, kLean = -6;
constexpr float kNib = 4.5f, kNibAngle = -40, kNibThickness = 0.7f;
// The palette's inks, darkest browns (the B's strokes), and the browns its edges are drawn in.
constexpr int kInks[] = {0, 1, 7, 8, 9, 10, 11};
constexpr int kEdges[] = {2, 6};
constexpr int kStroke[] = {9, 10, 7, 9, 11};  // the inks a stroke is written in, the commonest twice

using Grid = std::array<bool, N * N>;
using Rgb = std::array<float, 3>;

int at(int x, int y) { return y * N + x; }
bool in(int x, int y) { return x >= 0 && y >= 0 && x < N && y < N; }

bool in_box(int x, int y) { return x >= kLeft && x <= kRight && y >= kTop && y <= kBottom && (y >= 24 || x >= 17); }
bool on_plaque(int x, int y) { return x >= 8 && x <= 55 && y >= 7 && y <= 56; }

// Grown by `times` texels, to all eight neighbours, or to the four beside it.
Grid dilate(Grid g, int times = 1, bool diagonals = true) {
    for (int t = 0; t < times; t++) {
        const Grid prev = g;
        for (int y = 0; y < N; y++)
            for (int x = 0; x < N; x++)
                for (int j = -1; j <= 1; j++)
                    for (int i = -1; i <= 1; i++)
                        if ((diagonals || i == 0 || j == 0) && in(x + i, y + j) && prev[at(x + i, y + j)]) g[at(x, y)] = true;
    }
    return g;
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

// How much of each texel the O covers, from 4x4 samples: the nib drawn round its ellipse.
std::array<float, N * N> o_cover() {
    constexpr int kSteps = 720;
    const float lean = kLean * float(M_PI) / 180, nib = kNibAngle * float(M_PI) / 180;
    std::array<std::array<float, 2>, kSteps> path;
    for (int k = 0; k < kSteps; k++) {
        const float t = 2 * float(M_PI) * k / kSteps;
        const float ex = kRadiusX * std::cos(t), ey = kRadiusY * std::sin(t);
        path[k] = {kOX + ex * std::cos(lean) - ey * std::sin(lean), kOY + ex * std::sin(lean) + ey * std::cos(lean)};
    }
    const float ux = std::cos(nib), uy = std::sin(nib);
    auto inked = [&](float x, float y) {
        for (const auto& p : path) {
            const float dx = x - p[0], dy = y - p[1];
            if (std::abs(dx * ux + dy * uy) <= kNib / 2 && std::abs(dy * ux - dx * uy) <= kNibThickness / 2) return true;
        }
        return false;
    };
    std::array<float, N * N> cover{};
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            if (std::abs(x + 0.5f - kOX) > kRadiusY + kNib + 1 || std::abs(y + 0.5f - kOY) > kRadiusY + kNib + 1) continue;
            int count = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) count += inked(x + (sx + 0.5f) / 4, y + (sy + 0.5f) / 4);
            cover[at(x, y)] = count / 16.0f;
        }
    return cover;
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

std::optional<Texture> circle_rug_pad() {
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
    auto is_ink = [&](int i) { return std::find(std::begin(kInks), std::end(kInks), index[i]) != std::end(kInks); };
    auto is_edge = [&](int i) { return std::find(std::begin(kEdges), std::end(kEdges), index[i]) != std::end(kEdges); };

    // 1. The B: the ink joined to its stem, then ink close by (its curl's tail, the ends of its
    // bowls), then the browns round it.
    Grid ink{};
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) ink[at(x, y)] = in_box(x, y) && is_ink(at(x, y));
    Grid letter = part_at(ink, kStemX, kStemY);
    if (!letter[at(kStemX, kStemY)]) return std::nullopt;
    for (int t = 0; t < 3; t++) {
        const Grid near = dilate(letter, 3);
        for (int i = 0; i < N * N; i++) letter[i] = letter[i] || (ink[i] && near[i]);
    }
    for (int t = 0; t < 2; t++) {
        const Grid beside = dilate(letter, 1, false);
        for (int i = 0; i < N * N; i++)
            letter[i] = letter[i] || (beside[i] && in_box(i % N, i / N) && (is_ink(i) || is_edge(i)));
    }

    // 2. The B taken out and filled from its edges inward, each step copying the 3x3 patch of the
    // plaque (clear of the B and of ink) that best matches the known texels round it at about the
    // same height.
    const Grid hole = dilate(letter);
    const Grid away = dilate(hole, 2);
    std::vector<int> patches;  // their middles
    for (int y = 1; y < N - 1; y++)
        for (int x = 1; x < N - 1; x++) {
            bool all = true;
            for (int j = -1; j <= 1; j++)
                for (int i = -1; i <= 1; i++)
                    all = all && on_plaque(x + i, y + j) && !away[at(x + i, y + j)] && !is_ink(at(x + i, y + j));
            if (all) patches.push_back(at(x, y));
        }
    if (patches.size() < 2) return std::nullopt;
    constexpr float kHeight = 250;  // how much a row apart counts against a patch
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

    // 3. The O: where the nib covers most of a texel, one of the B's inks; at its edges the ink
    // blended into the plaque, as the nearest colour in the palette.
    const std::array<float, N * N> cover = o_cover();
    Rgb mean_ink{};
    for (int i : {9, 10, 7})
        for (int c = 0; c < 3; c++) mean_ink[c] += palette[i][c] / 3;
    std::mt19937 nib_random(5);
    for (int i = 0; i < N * N; i++) {
        const float c = cover[i];
        if (c <= 0.15f) continue;
        const Rgb ink_colour = c > 0.8f ? palette[kStroke[nib_random() % std::size(kStroke)]] : mean_ink;
        Rgb want;
        for (int k = 0; k < 3; k++) want[k] = palette[out[i]][k] * (1 - c) + ink_colour[k] * c;
        int nearest = 0;
        float nearest_d = 1e30f;
        for (int p = 0; p < 16; p++) {
            if (!opaque[p]) continue;
            float d = 0;
            for (int k = 0; k < 3; k++) d += (palette[p][k] - want[k]) * (palette[p][k] - want[k]);
            if (d < nearest_d) nearest_d = d, nearest = p;
        }
        out[i] = nearest;
    }

    Texture texture{hash(resource), N, N, std::vector<uint8_t>(N * N * 4)};
    for (int i = 0; i < N * N; i++) {
        for (int c = 0; c < 3; c++) texture.rgba[i * 4 + c] = uint8_t(palette[out[i]][c]);
        texture.rgba[i * 4 + 3] = opaque[out[i]] ? 255 : 0;
    }
    return texture;
}

} // namespace conker::pad_icons
