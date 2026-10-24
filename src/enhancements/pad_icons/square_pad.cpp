// The square B pads (as in scene 18) are a 64x64 CI4 texture with a 16-colour RGBA16 palette, flat
// resource 92. Across its top runs a wooden band, down to y 27; below it a pale metal plate, its
// sides and front a U-shaped rim, between dark wooden panels. On it stands a big raised B in the
// same pale pink-grey metal, lit from below: a dimmer edge along its top and left, a near-white line
// along the bottoms of its bars and the right of its bowls, a thin dark line round it, and its
// shadow falling upward, a dark band on the wood above it and the dark bottoms of its counters
// (which show the wood or the plate behind them).
//
// For a PlayStation pad the B and its shadow are taken out: above y 27 the wooden band is filled in
// from its own wood, below it the plate from the B's and the rim's metal, each from the edges
// inward, each step copying the 3x3 patch of its own kind that best matches what is already round
// it. Then an O stands where the B was, drawn as the B is: each texel of its ring copied from Rare's
// lines of texels across the B's strokes, from the edge of the B that faces the same way, at the
// same depth in; a thin dark line round it from the B's; and its shadow cast upward in the B's
// shadow texels, over the wood above it and the bottom of its counter. It is made from the player's
// ROM when the game starts, so no part of Rare's texture is in this repository.
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

constexpr int kResource = 92;
constexpr int N = 64;
constexpr int kTexelBytes = N * N / 2;
constexpr uint32_t kTlutRgba16 = 0x8000;  // G_TT_RGBA16, as RT64 hashes a TLUT draw

// The B stands in x 15-47, y 11-53 (inclusive). The wood above y 27 is filled from y 3 in x 12-49,
// the plate below it in x 14-47 down to y 54, the rim's metal left as it is. The O: its middle,
// outer radius and stroke width, in texels, and how far its shadow falls: so many texels dark,
// then so many of its edge.
constexpr int kLeft = 15, kTop = 11, kRight = 47, kBottom = 53;
constexpr int kWoodEnd = 27, kFillTop = 3, kWoodLeft = 12, kWoodRight = 49;
constexpr int kPlateLeft = 14, kPlateRight = 47, kPlateBottom = 54;
constexpr float kOX = 31.0f, kOY = 33.0f, kOuter = 14.0f, kWidth = 7.0f;
constexpr int kShadow = 3, kShadowEdge = 3;
constexpr float kPale = 100;     // the least mean lightness of the metal
constexpr float kHighlight = 200;  // the B's near-white line, left out of the plate's metal

using Rgb = std::array<float, 3>;
using Grid = std::array<bool, N * N>;

int at(int x, int y) { return y * N + x; }
bool in(int x, int y) { return x >= 0 && y >= 0 && x < N && y < N; }
float lum(const Rgb& c) { return (c[0] + c[1] + c[2]) / 3; }

// The four-neighbour dilation and erosion scipy.ndimage uses by default, `times` times.
Grid dilate(Grid g, int times = 1) {
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

Grid erode(const Grid& g, int times = 1) {
    Grid inverse;
    for (size_t i = 0; i < g.size(); i++) inverse[i] = !g[i];
    const Grid grown = dilate(inverse, times);
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

// A line of the B's texels from the edge texel (x, y) into its stroke along (dx, dy).
struct Line {
    int x, y, dx, dy;
};

} // namespace

std::optional<Texture> circle_square_pad() {
    const std::vector<uint8_t> resource = menu_art::load_resource(kResource);
    if (resource.size() != size_t(kTexelBytes + 32)) return std::nullopt;

    // The plate as RT64 decodes it (odd rows are word-swapped).
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
    std::vector<Rgb> rgb(N * N);
    std::vector<uint8_t> alpha(N * N);
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            const int byte = (y * N + x) / 2 ^ ((y & 1) ? 4 : 0);
            const int index = (x & 1) ? resource[byte] & 15 : resource[byte] >> 4;
            rgb[at(x, y)] = palette[index];
            alpha[at(x, y)] = opaque[index] ? 255 : 0;
        }

    // 1. The B: the pale metal in its box joined to its stem (the rim's metal touches it only
    // below the box), closed over the odd dark texel in it. The rim's metal is the rest of the
    // pale metal on the plate.
    Grid box{}, metal{}, rim{};
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            box[at(x, y)] = x >= kLeft && x <= kRight && y >= kTop && y <= kBottom;
            metal[at(x, y)] = box[at(x, y)] && lum(rgb[at(x, y)]) > kPale;
        }
    const Grid face = part_at(metal, kLeft + 2, (kTop + kBottom) / 2);
    Grid letter = erode(dilate(face));
    for (int i = 0; i < N * N; i++) {
        letter[i] = face[i] || (letter[i] && box[i]);
        rim[i] = lum(rgb[i]) > kPale && !letter[i] && i / N >= kWoodEnd;
    }

    // 2. The B and its shadow taken out: above kWoodEnd the wood, below it the plate, each from
    // the edges inward, each step copying the 3x3 patch of its own kind that best matches the
    // known texels of its kind round it. The wood comes from the band clear of the B's shadow, the
    // plate's metal from the B's face (but its near-white line) and the rim.
    Grid hole{};
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            const int i = at(x, y);
            const bool plate = y >= kFillTop && y <= kPlateBottom && x >= kPlateLeft && x <= kPlateRight && !rim[i];
            const bool wood = y >= kFillTop && y < kWoodEnd && x >= kWoodLeft && x <= kWoodRight;
            hole[i] = letter[i] || plate || wood;
        }
    auto is_wood = [](int i) { return i / N < kWoodEnd; };
    const Grid away = dilate(hole, 3);
    const Grid face_metal = erode([&] {
        Grid g{};
        for (int i = 0; i < N * N; i++) g[i] = letter[i] && lum(rgb[i]) > kPale;
        return g;
    }());
    constexpr int kHalf = 1;
    std::vector<int> wood_patches, metal_patches;  // their middles
    for (int y = kHalf; y < N - kHalf; y++)
        for (int x = kHalf; x < N - kHalf; x++) {
            bool wood = true, plate = true;
            for (int j = -kHalf; j <= kHalf; j++)
                for (int i = -kHalf; i <= kHalf; i++) {
                    const int t = at(x + i, y + j);
                    wood = wood && is_wood(t) && y + j >= kFillTop && !away[t];
                    plate = plate && ((face_metal[t] && lum(rgb[t]) < kHighlight) || rim[t]);
                }
            if (wood) wood_patches.push_back(at(x, y));
            if (plate) metal_patches.push_back(at(x, y));
        }
    if (wood_patches.empty() || metal_patches.empty()) return std::nullopt;
    std::mt19937 random(3);
    std::vector<Rgb> out = rgb;
    Grid known;
    for (int i = 0; i < N * N; i++) known[i] = !hole[i];
    for (;;) {
        // The texel to fill beside known texels with the most known texels of its kind round it.
        int best_i = -1, best_known = -1;
        for (int y = 0; y < N; y++)
            for (int x = 0; x < N; x++) {
                if (known[at(x, y)]) continue;
                bool front = false;
                const int n[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
                for (const auto& p : n) front = front || (in(p[0], p[1]) && known[at(p[0], p[1])]);
                if (!front) continue;
                int count = 0;
                for (int j = -kHalf; j <= kHalf; j++)
                    for (int i = -kHalf; i <= kHalf; i++)
                        count += in(x + i, y + j) && known[at(x + i, y + j)] && is_wood(at(x + i, y + j)) == is_wood(at(x, y));
                if (count > best_known) best_known = count, best_i = at(x, y);
            }
        if (best_i < 0) break;
        const int x = best_i % N, y = best_i / N;
        const bool wood = is_wood(best_i);
        std::vector<std::pair<float, int>> scores;
        for (int p : wood ? wood_patches : metal_patches) {
            float d = 0;
            for (int j = -kHalf; j <= kHalf; j++)
                for (int i = -kHalf; i <= kHalf; i++) {
                    if (!in(x + i, y + j) || !known[at(x + i, y + j)] || is_wood(at(x + i, y + j)) != wood) continue;
                    const Rgb& a = out[at(x + i, y + j)];
                    const Rgb& b = rgb[p + j * N + i];
                    for (int c = 0; c < 3; c++) d += (a[c] - b[c]) * (a[c] - b[c]);
                }
            scores.emplace_back(d, p);
        }
        std::partial_sort(scores.begin(), scores.begin() + std::min<size_t>(2, scores.size()), scores.end());
        const int chosen = scores[random() % std::min<size_t>(2, scores.size())].second;
        for (int j = -kHalf; j <= kHalf; j++)
            for (int i = -kHalf; i <= kHalf; i++) {
                const int t = at(x + i, y + j);
                if (!in(x + i, y + j) || known[t] || is_wood(t) != wood) continue;
                out[t] = rgb[chosen + j * N + i];
                known[t] = true;
            }
    }

    // 3. The O's shadow, cast upward as the B's is: where the O moved up 1 to kShadow texels lies,
    // the B's darkest shadow (just above it), then its lighter edge (further up).
    Grid o{};
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            int inside = 0;
            for (int sy = 0; sy < 8; sy++)
                for (int sx = 0; sx < 8; sx++) {
                    const float d = std::hypot(x + (sx + 0.5f) / 8 - kOX, y + (sy + 0.5f) / 8 - kOY);
                    inside += d >= kOuter - kWidth && d <= kOuter;
                }
            o[at(x, y)] = inside > 64 * 0.45f;
        }
    std::vector<Rgb> shadow, shadow_edge, line_above, line_below;
    for (int y = 3; y <= 10; y++)
        for (int x = 16; x <= 44; x++) (y >= 7 ? shadow : shadow_edge).push_back(rgb[at(x, y)]);
    for (int y = 15; y <= 52; y++) line_above.push_back(rgb[at(kLeft - 1, y)]);  // left of the stem
    for (int x = 18; x <= 36; x++) line_below.push_back(rgb[at(x, kBottom + 1)]);  // under the bottom bar
    Grid dark{}, edge{};
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++)
            for (int k = 1; k <= kShadow + kShadowEdge; k++)
                if (in(x, y + k) && o[at(x, y + k)]) (k <= kShadow ? dark : edge)[at(x, y)] = true;
    for (int i = 0; i < N * N; i++) {
        if (o[i]) continue;
        if (dark[i]) out[i] = shadow[random() % shadow.size()];
        else if (edge[i]) out[i] = shadow_edge[random() % shadow_edge.size()];
    }

    // 4. The dark line round it, as round the B: browner under the edges facing down, as under
    // its bottom bar, and as left of its stem elsewhere (but in the shadow).
    const float middle = kOuter - kWidth / 2;
    const Grid around = dilate(o);
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            const int i = at(x, y);
            if (!around[i] || o[i] || dark[i]) continue;
            const float dx = x + 0.5f - kOX, dy = y + 0.5f - kOY, d = std::hypot(dx, dy);
            const float down = (d > middle ? dy : -dy) / d;  // how much the O's edge there faces down
            const std::vector<Rgb>& from = down > 0.3f ? line_below : line_above;
            out[i] = from[random() % from.size()];
        }

    // 5. The ring: Rare's lines across the B's strokes, by the way the edge faces. Outer edges
    // from the B's outside (its top bar's top, its bottom bar's bottom, its stem's left, its bowls'
    // right), inner ones from its counters' (its bars' bottoms and tops over and under them, its
    // stem's right, its bowls' insides). Near a diagonal the two facings are mixed texel by texel.
    auto first = [&](int x, int y, int dx, int dy, int steps) -> std::optional<Line> {
        for (int k = 0; k < steps; k++, x += dx, y += dy)
            if (in(x, y) && letter[at(x, y)]) return Line{x, y, dx, dy};
        return std::nullopt;
    };
    // [outer, inner][right, left, down, up]: the way the edge faces out of the stroke.
    std::array<std::array<std::vector<Line>, 4>, 2> runs;
    auto add = [](std::vector<Line>& to, std::optional<Line> line) {
        if (line) to.push_back(*line);
    };
    // A scan from outside the stroke finds its edge texel; the line then runs on the way it went.
    for (int x = 18; x < 38; x++) add(runs[0][3], first(x, 10, 0, 1, 10));
    for (int x = 18; x < 36; x++) add(runs[0][2], first(x, 53, 0, -1, 12));
    for (int y = 12; y < 52; y++)
        if (y < 30 || y >= 36) add(runs[0][1], first(15, y, 1, 0, 15));
    for (int y = 15; y < 49; y++)
        if (y < 28 || y >= 39) add(runs[0][0], first(47, y, -1, 0, 17));
    for (int x = 25; x < 36; x++) {
        for (int y0 : {20, 42}) add(runs[1][2], first(x, y0, 0, -1, 12));  // bars' bottoms, from the counters
        for (int y0 : {24, 40}) add(runs[1][3], first(x, y0, 0, 1, 12));   // bars' tops, from the counters
    }
    for (int y = 20; y < 46; y++) {
        if (y >= 28 && y < 40) continue;
        add(runs[1][0], first(29, y, -1, 0, 15));  // the stem's right side, from the counters
        add(runs[1][1], first(29, y, 1, 0, 17));   // the bowls' insides, from the counters
    }
    for (const auto& side : runs)
        for (const auto& run : side)
            if (run.empty()) return std::nullopt;
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            const int i = at(x, y);
            if (!o[i]) continue;
            const float dx = x + 0.5f - kOX, dy = y + 0.5f - kOY, r = std::hypot(dx, dy);
            const float turn = std::atan2(dy, dx);
            const bool outer = r >= middle;
            const float a = outer ? turn : turn + float(M_PI);  // the way the nearer edge faces
            const float depth = outer ? kOuter - r : r - (kOuter - kWidth);
            const float radius = outer ? kOuter : kOuter - kWidth;
            const float c = std::cos(a), s = std::sin(a);
            const float weights[4] = {std::pow(std::max(c, 0.0f), 12.0f), std::pow(std::max(-c, 0.0f), 12.0f),
                                      std::pow(std::max(s, 0.0f), 12.0f), std::pow(std::max(-s, 0.0f), 12.0f)};
            const float total = weights[0] + weights[1] + weights[2] + weights[3];
            double hashed = std::fmod(std::sin(x * 12.9898 + y * 78.233) * 43758.5453, 1.0);
            float pick = float(hashed < 0 ? hashed + 1 : hashed) * total;
            int facing = 0;
            while (facing < 3 && pick >= weights[facing]) pick -= weights[facing++];
            const std::vector<Line>& run = runs[outer ? 0 : 1][facing];
            const Line& line = run[size_t((turn + float(M_PI)) * radius) % run.size()];
            const int k = std::min(int(depth), 7);
            const int sx = line.x + line.dx * k, sy = line.y + line.dy * k;
            if (in(sx, sy)) out[i] = rgb[at(sx, sy)];
        }

    Texture texture{hash(resource), N, N, std::vector<uint8_t>(N * N * 4)};
    for (int i = 0; i < N * N; i++) {
        for (int c = 0; c < 3; c++) texture.rgba[i * 4 + c] = uint8_t(std::lround(out[i][c]));
        texture.rgba[i * 4 + 3] = alpha[i];
    }
    return texture;
}

} // namespace conker::pad_icons
