// The stone B pads (as in Windy) are a round marble disc split into two 32x64 CI8 textures with
// their own 256-colour RGBA16 palettes, flat resources 509 (left) and 510 (right). The B is the
// disc's own marble, raised: a dark band along its top and left, dark counters, a faint blue-grey
// line along its right and bottom and a white highlight on its edge facing down and right. A ring
// round it has a shadow along its top left.
//
// For a PlayStation pad the B's lines are taken out: where they cross the ring, the ring and its
// shadow are rebuilt from their mirror image across the light's diagonal; inside the ring the gap
// is filled from its edges inward with 5x5 patches of the disc's own light marble, each the one
// that best matches the stone already round it, so the veins run on. Then a square is drawn in the
// middle of the ring, edged the way the B is, in the B's own colours, its face lifted a little
// towards the B's lighter marble. It is made from the player's ROM when the game starts, so no part
// of Rare's texture is in this repository.
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

constexpr int kHalves[2] = {509, 510};  // left, right
constexpr int kW = 32, kH = 64;         // each half
constexpr int N = 64;                   // the whole disc
constexpr int kTexelBytes = kW * kH;
constexpr int kResourceBytes = kTexelBytes + 256 * 2;
constexpr uint32_t kTlutRgba16 = 0x8000;  // G_TT_RGBA16, as RT64 hashes a TLUT draw

// The disc's middle (fitted to its rim), the ring round the B, and the square: its middle (fitted to the
// ring), outer half-width and stroke width, in texels.
constexpr float kCX = 31.33f, kCY = 31.51f, kRing = 17.0f;
constexpr float kOX = 32.3f, kOY = 32.5f, kOuter = 12.5f, kWidth = 6.5f;
constexpr float kLight = 165.0f;  // the least mean lightness of a marble patch to fill from

using Rgb = std::array<float, 3>;
using Grid = std::array<bool, N * N>;

int at(int x, int y) { return y * N + x; }
bool in(int x, int y) { return x >= 0 && y >= 0 && x < N && y < N; }
float lum(const Rgb& c) { return (c[0] + c[1] + c[2]) / 3; }

// The four-neighbour dilation scipy.ndimage uses by default, `times` times.
Grid dilate(Grid g, int times) {
    for (int t = 0; t < times; t++) {
        const Grid prev = g;
        for (int y = 0; y < N; y++)
            for (int x = 0; x < N; x++) {
                bool v = prev[at(x, y)];
                const int n[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
                for (const auto& p : n) v = v || (in(p[0], p[1]) && prev[at(p[0], p[1])]);
                g[at(x, y)] = v;
            }
    }
    return g;
}

// `g` moved by (dx, dy), wrapping as numpy's roll does.
Grid roll(const Grid& g, int dx, int dy) {
    Grid out{};
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) out[at(((x + dx) % N + N) % N, ((y + dy) % N + N) % N)] = g[at(x, y)];
    return out;
}

// Texels more than `coverage` covered by the square's stroke between reaches `inner` and `outer`
// about (cx, cy).
Grid square(float cx, float cy, float inner, float outer, float coverage) {
    Grid out{};
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            int inside = 0;
            for (int sy = 0; sy < 8; sy++)
                for (int sx = 0; sx < 8; sx++) {
                    const float d = square_symbol::reach(x + (sx + 0.5f) / 8 - cx, y + (sy + 0.5f) / 8 - cy);
                    inside += d >= inner && d <= outer;
                }
            out[at(x, y)] = inside > 64 * coverage;
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

std::vector<Texture> stone_pad() {
    // The disc as RT64 decodes it.
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

    std::array<float, N * N> r, L;
    Grid inB{}, blue{}, strong{};
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            const int i = at(x, y);
            r[i] = std::hypot(x + 0.5f - kCX, y + 0.5f - kCY);
            L[i] = lum(rgb[i]);
            inB[i] = x + 0.5f > 19 && x + 0.5f < 49 && y + 0.5f > 9 && y + 0.5f < 50 && r[i] < 23;
            blue[i] = rgb[i][2] - rgb[i][0] > 12;
            strong[i] = L[i] < 110 && inB[i];
        }

    // 1. The B's lines: its dark band and counters, the fainter blue-grey line round the rest, and
    // the white highlight beside it.
    const Grid near_strong = dilate(strong, 4);
    Grid lines{}, highlight{}, mask{};
    for (int i = 0; i < N * N; i++) lines[i] = inB[i] && (strong[i] || (L[i] < 150 && blue[i] && near_strong[i]));
    const Grid near_lines = dilate(lines, 2);
    for (int i = 0; i < N * N; i++) highlight[i] = L[i] > 235 && near_lines[i] && inB[i];
    Grid either{};
    for (int i = 0; i < N * N; i++) either[i] = lines[i] || highlight[i];
    const Grid grown = dilate(either, 1);
    for (int i = 0; i < N * N; i++) mask[i] = grown[i] && inB[i];

    // Where the B crosses the ring (and its shadow), the ring from its mirror image across the
    // light's diagonal: lit from the top left, a point and its mirror there are lit alike.
    std::vector<Rgb> out = rgb;
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            if (!mask[at(x, y)] || r[at(x, y)] < kRing) continue;
            const int ix = int(std::floor(kCX + (y + 0.5f - kCY))), iy = int(std::floor(kCY + (x + 0.5f - kCX)));
            if (in(ix, iy) && !mask[at(ix, iy)]) out[at(x, y)] = rgb[at(ix, iy)];
        }
    Grid hole{};
    for (int i = 0; i < N * N; i++) hole[i] = mask[i] && r[i] < kRing;

    // 2. The gap inside the ring, from its edges inward, each step copying the 5x5 patch of light
    // marble (wholly inside the ring, clear of the B) that best matches the stone already round it.
    constexpr int kHalf = 2, kSide = 2 * kHalf + 1;
    struct Patch {
        int x, y;
    };
    std::vector<Patch> patches;
    for (int y = kHalf; y < N - kHalf; y++)
        for (int x = kHalf; x < N - kHalf; x++) {
            bool clear = true;
            float sum = 0;
            for (int j = -kHalf; j <= kHalf && clear; j++)
                for (int i = -kHalf; i <= kHalf && clear; i++) {
                    const int k = at(x + i, y + j);
                    clear = r[k] < kRing - 0.5f && !lines[k] && !highlight[k] && !hole[k];
                    sum += L[k];
                }
            if (clear && sum / (kSide * kSide) > kLight) patches.push_back({x, y});
        }
    std::mt19937 random(3);
    if (patches.empty()) return {};
    auto mirror = [](int v) { return v < 0 ? -v - 1 : v >= N ? 2 * N - v - 1 : v; };
    for (;;) {
        // The hole texel beside known stone with the most known stone round it.
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
        std::vector<std::pair<float, size_t>> scores;
        for (size_t p = 0; p < patches.size(); p++) {
            float d = 0;
            for (int j = -kHalf; j <= kHalf; j++)
                for (int i = -kHalf; i <= kHalf; i++) {
                    if (!in(x + i, y + j) || hole[at(x + i, y + j)]) continue;
                    const Rgb& a = out[at(x + i, y + j)];
                    const Rgb& b = out[at(patches[p].x + i, patches[p].y + j)];
                    for (int c = 0; c < 3; c++) d += (a[c] - b[c]) * (a[c] - b[c]);
                }
            scores.emplace_back(d, p);
        }
        std::partial_sort(scores.begin(), scores.begin() + std::min<size_t>(3, scores.size()), scores.end());
        const Patch& chosen = patches[scores[random() % std::min<size_t>(3, scores.size())].second];
        for (int j = -kHalf; j <= kHalf; j++)
            for (int i = -kHalf; i <= kHalf; i++) {
                if (!in(x + i, y + j) || !hole[at(x + i, y + j)]) continue;
                out[at(x + i, y + j)] = out[at(chosen.x + i, chosen.y + j)];
                hole[at(x + i, y + j)] = false;
            }
    }

    // 3. The square, edged as the B is: a solid dark band two texels wide where the stroke lies down and
    // to the right of a texel (darker still in the hole, as the B's counters), a faint blue-grey
    // line one texel outside its edges facing down and right, a white highlight on its own edge
    // facing down and right.
    const Grid o = square(kOX, kOY, kOuter - kWidth, kOuter, 0.45f);
    const Grid inside = square(kOX, kOY, 0, kOuter - kWidth, 0.5f);
    const Grid down_right1 = roll(o, -1, -1), down_right2 = roll(o, -2, -2);
    std::vector<Rgb> band_colours, counters, whites, faint_colours, face;
    for (int y = 18; y < 46; y++)
        for (int x : {22, 23})
            if (lum(rgb[at(x, y)]) < 80) band_colours.push_back(rgb[at(x, y)]);
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            const int i = at(x, y);
            if (!inB[i]) continue;
            if (L[i] < 60) counters.push_back(rgb[i]);
            if (L[i] > 225) whites.push_back(rgb[i]);
            if (L[i] > 120 && L[i] < 160 && blue[i] && x + 0.5f > 42) faint_colours.push_back(rgb[i]);
            if (L[i] > 175 && L[i] < 225 && !mask[i]) face.push_back(rgb[i]);
        }
    if (band_colours.empty() || counters.empty() || whites.empty() || faint_colours.empty() || face.empty()) return {};
    auto mean = [](const std::vector<Rgb>& v) {
        Rgb m{};
        for (const Rgb& c : v)
            for (int k = 0; k < 3; k++) m[k] += c[k] / v.size();
        return m;
    };
    std::vector<Rgb> stroke;
    for (int i = 0; i < N * N; i++)
        if (o[i]) stroke.push_back(out[i]);
    const Rgb face_mean = mean(face), stroke_mean = mean(stroke);
    std::vector<Rgb> marked = out;
    for (int i = 0; i < N * N; i++)
        if (o[i])
            for (int k = 0; k < 3; k++) marked[i][k] = std::clamp(out[i][k] + (face_mean[k] - stroke_mean[k]) * 0.5f, 0.0f, 255.0f);
    auto pick = [&](const std::vector<Rgb>& v) { return v[random() % v.size()]; };
    Grid band{};
    for (int i = 0; i < N * N; i++) band[i] = (down_right1[i] || down_right2[i]) && !o[i];
    for (int i = 0; i < N * N; i++)
        if (band[i]) marked[i] = pick(inside[i] ? counters : band_colours);
    const Grid below = roll(o, 0, 1), right = roll(o, 1, 0);
    for (int i = 0; i < N * N; i++)
        if ((below[i] || right[i]) && !o[i] && !band[i] && !inside[i]) marked[i] = pick(faint_colours);
    for (int i = 0; i < N * N; i++)
        if (o[i] && !down_right1[i]) marked[i] = pick(whites);

    std::vector<Texture> textures;
    for (int h = 0; h < 2; h++) {
        Texture t{hash(halves[h].resource), kW, kH, std::vector<uint8_t>(kW * kH * 4)};
        for (int y = 0; y < kH; y++)
            for (int x = 0; x < kW; x++) {
                const int i = at(h * kW + x, y);
                uint8_t* p = &t.rgba[(y * kW + x) * 4];
                for (int c = 0; c < 3; c++) p[c] = uint8_t(std::lround(marked[i][c]));
                p[3] = alpha[i];
            }
        textures.push_back(std::move(t));
    }
    return textures;
}

} // namespace conker::pad_icons
