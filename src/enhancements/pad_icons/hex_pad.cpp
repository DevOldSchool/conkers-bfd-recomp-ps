// The hexagonal wooden B pads (as in scene 12) are a wooden hexagon split into two 32x64 CI8
// textures with their own 256-colour RGBA16 palettes, flat resources 1573 (left) and 1574 (right).
// Its grain runs in hexagonal rings round a plate in the middle, and on the plate stands a raised B
// in pale green, lit from the lower right: one near-black texel and two dark green ones along its
// edges facing up and left (inside its counters too), a pale highlight two texels wide on those
// facing down and right.
//
// For a PlayStation pad or the keyboard the B is taken out and the plate under it filled from the same ring of the
// grain further along the hexagon's edge, so the rings run on through it; the mark's counter (the
// middle, where every ring is under the B) from one piece of the plate's smoothest wood, and what
// is left from 5x5 patches of the plate. Then the mark (a square, or an E for the keyboard) stands where the B was, edged texel for
// texel as the B is, with the B's own edge and face texels. It is made from the player's ROM when
// the game starts, so no part of Rare's texture is in this repository.
#include "enhancements/pad_icons/b_pad.hpp"
#include "enhancements/pad_icons/ring_fill.hpp"
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

constexpr int kHalves[2] = {1573, 1574};  // left, right
constexpr int kW = 32, kH = 64;           // each half
constexpr int N = 64;                     // the whole hexagon
constexpr int kTexelBytes = kW * kH;
constexpr int kResourceBytes = kTexelBytes + 256 * 2;
constexpr uint32_t kTlutRgba16 = 0x8000;  // G_TT_RGBA16, as RT64 hashes a TLUT draw

// The hexagon (its outline against the black round it): its middle, half its width (to the corners
// left and right), half its height and half the length of its top and bottom edges; the plate is
// inside kPlate of that. Then the mark, in the hexagon's middle (Rare's B stands two texels right of
// it): its middle, outer half-width and stroke width, in texels.
constexpr float kCX = 32.0f, kCY = 32.5f, kHalfWidth = 32.0f, kHalfHeight = 30.5f, kHalfTop = 17.5f;
constexpr float kPlate = 0.65f;
constexpr float kOX = 32.0f, kOY = 32.5f, kOuter = 14.0f, kWidth = 7.5f;

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

// Only the parts of `g` larger than `size` texels (four-connected).
Grid large_parts(const Grid& g, int size) {
    Grid out{};
    std::array<int, N * N> part{};
    int next = 0;
    for (int start = 0; start < N * N; start++) {
        if (!g[start] || part[start]) continue;
        std::vector<int> members{start};
        part[start] = ++next;
        for (size_t k = 0; k < members.size(); k++) {
            const int x = members[k] % N, y = members[k] / N;
            const int n[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
            for (const auto& p : n) {
                if (in(p[0], p[1]) && g[at(p[0], p[1])] && !part[at(p[0], p[1])]) {
                    part[at(p[0], p[1])] = next;
                    members.push_back(at(p[0], p[1]));
                }
            }
        }
        if (int(members.size()) > size) {
            for (int m : members) out[m] = true;
        }
    }
    return out;
}

// How far out a texel is, by the hexagon's rings: 1 on its edge.
float hex_ring(int x, int y) {
    const float dx = std::abs(x + 0.5f - kCX), dy = std::abs(y + 0.5f - kCY);
    return std::max(dy / kHalfHeight, (dx + dy * (kHalfWidth - kHalfTop) / kHalfHeight) / kHalfWidth);
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

std::vector<Texture> hex_pad(Mark::Shape shape) {
    const Mark mark{shape, kOuter, kWidth};
    // The hexagon as RT64 decodes it.
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

    // 1. The B: its pale green face, and next to it its dark band and counters and the greener
    // texels of its edges.
    std::array<float, N * N> L, green;
    Grid box{}, face{};
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            const int i = at(x, y);
            L[i] = lum(rgb[i]);
            green[i] = rgb[i][1] - rgb[i][0];
            box[i] = x + 0.5f > 18 && x + 0.5f < 51 && y + 0.5f > 12 && y + 0.5f < 52;
            face[i] = green[i] > 0 && L[i] > 100 && box[i];
        }
    face = large_parts(face, 30);  // the letter, not greenish specks in the wood
    const Grid near = dilate(face, 4);
    Grid letter{};
    for (int i = 0; i < N * N; i++) letter[i] = face[i] || (near[i] && box[i] && (L[i] < 70 || green[i] > -2));
    const Grid closed = erode(dilate(letter));
    for (int i = 0; i < N * N; i++) letter[i] = letter[i] || closed[i];
    Grid hole = dilate(letter);
    for (int i = 0; i < N * N; i++) hole[i] = hole[i] && box[i];

    // 2. The plate under it: its grain runs in hexagonal rings, so first from the same ring further
    // along the same edge of the hexagon; then what is left (where no ring is clear of the B), from
    // the edges inward, each step copying the 5x5 patch of the plate (clear of the B) that best
    // matches the wood already round it.
    std::vector<Rgb> out = rgb;
    // The mark's stroke (step 3 paints it over whatever is filled there).
    Grid o{};
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            int inside = 0;
            for (int sy = 0; sy < 8; sy++)
                for (int sx = 0; sx < 8; sx++) {
                    inside += mark.stroke(x + (sx + 0.5f) / 8 - kOX, y + (sy + 0.5f) / 8 - kOY);
                }
            o[at(x, y)] = inside > 64 * 0.45f;
        }
    {
        auto along = [](int x, int y) {
            const float dx = x + 0.5f - kCX, dy = y + 0.5f - kCY;
            if (std::abs(dy) / kHalfHeight >= (std::abs(dx) + std::abs(dy) * (kHalfWidth - kHalfTop) / kHalfHeight) / kHalfWidth)
                return ring_fill::Step{1, 0};  // the top and bottom edges
            const float ex = kHalfWidth - kHalfTop, ey = kHalfHeight * (dx * dy < 0 ? 1 : -1), length = std::hypot(ex, ey);
            return ring_fill::Step{ex / length, ey / length};
        };
        auto same_edge = [&](int sx, int sy, int x, int y) {
            const ring_fill::Step a = along(sx, sy), b = along(x, y);
            return a.x * b.x + a.y * b.y > 0.99f;
        };
        Grid filled{};
        ring_fill::fill(
            N, N, [&](int x, int y) { return hole[at(x, y)]; }, along,
            [&](int sx, int sy, int x, int y) {
                return !hole[at(sx, sy)] && alpha[at(sx, sy)] && same_edge(sx, sy, x, y) &&
                       std::abs(hex_ring(sx, sy) - hex_ring(x, y)) * kHalfHeight < 0.6f;
            },
            [&](int sx, int sy, int x, int y) { out[at(x, y)] = rgb[at(sx, sy)], filled[at(x, y)] = true; });
        ring_fill::fill_pieces(
            N, N, [&](int x, int y) { return hole[at(x, y)] && !filled[at(x, y)] && !o[at(x, y)]; },
            [&](int sx, int sy) { return !hole[at(sx, sy)] && hex_ring(sx, sy) < 0.8f; },
            [&](int sx, int sy) {
                float r = 0;
                if (sx + 1 < N) r += std::abs(lum(rgb[at(sx + 1, sy)]) - lum(rgb[at(sx, sy)]));
                if (sy + 1 < N) r += std::abs(lum(rgb[at(sx, sy + 1)]) - lum(rgb[at(sx, sy)]));
                return r;
            },
            [&](int sx, int sy, int x, int y) { out[at(x, y)] = rgb[at(sx, sy)], filled[at(x, y)] = true; });
        for (int i = 0; i < N * N; i++) hole[i] = hole[i] && !filled[i];
    }
    constexpr int kHalf = 2;
    struct Patch {
        int x, y;
    };
    std::vector<Patch> patches;
    for (int y = kHalf; y < N - kHalf; y++)
        for (int x = kHalf; x < N - kHalf; x++) {
            bool clear = true;
            for (int j = -kHalf; j <= kHalf && clear; j++)
                for (int i = -kHalf; i <= kHalf && clear; i++)
                    clear = !hole[at(x + i, y + j)] && hex_ring(x + i, y + j) < kPlate;
            if (clear) patches.push_back({x, y});
        }
    if (patches.empty()) return {};
    std::mt19937 random(3);
    auto mirror = [](int v) { return v < 0 ? -v - 1 : v >= N ? 2 * N - v - 1 : v; };
    for (;;) {
        // The hole texel beside known wood with the most known wood round it.
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

    // 3. The mark, raised and lit from the lower right as the B is. Where its edge faces up or
    // left it has the B's band there, texel for texel in from the edge: the near-black, then the
    // two dark greens; where it faces down or right, the B's two texels of highlight there, the
    // brightest where it faces the light squarely. Each is copied from the B's own edge, in order
    // round the mark, so the band runs on as the B's does. The rest is the B's own face: the same
    // texel where the face was, else the nearest.
    // How many texels in from the stroke's nearer edge each of its texels is.
    std::array<int, N * N> depth{};
    Grid left = o;
    for (int step = 1; std::find(left.begin(), left.end(), true) != left.end(); step++) {
        const Grid next = erode(left);
        for (int i = 0; i < N * N; i++)
            if (left[i] && !next[i]) depth[i] = step;
        left = next;
    }

    // The B's edge texels, by how far in from its edge: up the edges facing left (its stem) and
    // across those facing up (its top), the band; down those facing right and across those facing
    // down (its bowls and its bottom), the highlight, darkest first.
    std::array<std::vector<Rgb>, 4> band, shine;
    for (int y = 17; y < 46; y++)
        for (int x = 18; x < 30; x++)
            if (face[at(x, y)]) {
                for (int k = 1; k <= 3; k++) band[k].push_back(rgb[at(x - 4 + k, y)]);
                break;
            }
    for (int x = 23; x < 38; x++)
        for (int y = 13; y < 22; y++)
            if (face[at(x, y)]) {
                for (int k = 1; k <= 3; k++) band[k].push_back(rgb[at(x, y - 4 + k)]);
                break;
            }
    for (int y = 20; y < 48; y++)
        for (int x = 49; x >= 30; x--)
            if (face[at(x, y)]) {
                for (int k = 1; k <= 2; k++) shine[k].push_back(rgb[at(x + 1 - k, y)]);
                break;
            }
    for (int x = 23; x < 42; x++)
        for (int y = 51; y >= 38; y--)
            if (face[at(x, y)]) {
                for (int k = 1; k <= 2; k++) shine[k].push_back(rgb[at(x, y + 1 - k)]);
                break;
            }
    for (int k = 1; k <= 3; k++)
        if (band[k].empty()) return {};
    for (int k = 1; k <= 2; k++) {
        if (shine[k].empty()) return {};
        std::stable_sort(shine[k].begin(), shine[k].end(), [](const Rgb& a, const Rgb& b) { return lum(a) < lum(b); });
    }
    const Grid face_inside = erode(face, 2);
    std::vector<int> core;  // the B's face away from its edges
    for (int i = 0; i < N * N; i++)
        if (face_inside[i] && L[i] < 200) core.push_back(i);
    if (core.empty()) return {};

    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            const int i = at(x, y);
            if (!o[i]) continue;
            const float dx = x + 0.5f - kOX, dy = y + 0.5f - kOY;
            const square_symbol::Facing f = mark.out(dx, dy);
            const float lit = f.x + f.y;  // its edge facing the lower right
            const int along = int((std::atan2(dy, dx) + float(M_PI)) / (2 * float(M_PI)) * 97);
            const int k = depth[i];
            if (lit < 0 && k <= 3) {
                out[i] = band[k][along % band[k].size()];
            } else if (lit >= 0 && k <= 2) {
                const std::vector<Rgb>& v = shine[k];
                const float squarely = lit / std::sqrt(2.0f);
                const int pick = int(squarely * (v.size() - 1)) + along % 7 - 3;
                out[i] = v[std::clamp(pick, 0, int(v.size()) - 1)];
            } else if (face_inside[i] && L[i] < 200) {
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
