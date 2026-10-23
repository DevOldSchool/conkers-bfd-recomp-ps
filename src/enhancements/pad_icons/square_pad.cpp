// The square B pads (as in scene 18) are a 64x64 CI4 texture with a 16-colour RGBA16 palette, flat
// resource 92: a dark wooden plate with a pale U-shaped rim along its sides and front, and inside
// it a dark recess with a big raised B in pale pink-grey stone. The B is lit from below and the
// right: a dimmer edge along its top and left, a near-white one along the bottoms of its bars and
// the right of its bowls, and a dark line round it where it meets the recess.
//
// For a PlayStation pad the B is taken out and the recess under it filled from its edges inward,
// each step copying the 5x5 patch of the recess's own floor (clear of the B) that best matches what
// is already round it. Then an O stands where the B was, edged texel for texel as the B is, with
// the B's own edge, face and outline texels. It is made from the player's ROM when the game starts,
// so no part of Rare's texture is in this repository.
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

// The B stands in x 15-47, y 11-53 (inclusive), inside the recess, which runs from the plate's
// back to the rim's front. The O: its middle (halfway between the rim's sides), outer radius and
// stroke width, in texels.
constexpr int kLeft = 15, kTop = 11, kRight = 47, kBottom = 53;
constexpr int kRecessLeft = 14, kRecessTop = 3, kRecessRight = 47, kRecessBottom = 54;
constexpr float kOX = 30.5f, kOY = 32.5f, kOuter = 15.5f, kWidth = 7.5f;
constexpr float kPale = 100;  // the least mean lightness of the B's stone

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

    // 1. The B: the pale stone in its box joined to its stem (the rim's stone touches it only
    // below the box), closed over the odd dark texel in it.
    Grid box{}, stone{};
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            box[at(x, y)] = x >= kLeft && x <= kRight && y >= kTop && y <= kBottom;
            stone[at(x, y)] = box[at(x, y)] && lum(rgb[at(x, y)]) > kPale;
        }
    const Grid face = part_at(stone, kLeft + 2, (kTop + kBottom) / 2);
    Grid letter = erode(dilate(face));
    for (int i = 0; i < N * N; i++) letter[i] = face[i] || (letter[i] && box[i]);
    Grid hole = dilate(letter);
    for (int i = 0; i < N * N; i++) hole[i] = letter[i] || (hole[i] && box[i]);

    // 2. The recess under it, from the edges inward, each step copying the 5x5 patch of the
    // recess (clear of the B) that best matches what is already round it.
    constexpr int kHalf = 2;
    struct Patch {
        int x, y;
    };
    std::vector<Patch> patches;
    for (int y = kHalf; y < N - kHalf; y++)
        for (int x = kHalf; x < N - kHalf; x++) {
            bool clear = true;
            for (int j = -kHalf; j <= kHalf && clear; j++)
                for (int i = -kHalf; i <= kHalf && clear; i++) {
                    const int px = x + i, py = y + j;
                    clear = !hole[at(px, py)] && px >= kRecessLeft && px <= kRecessRight && py >= kRecessTop && py <= kRecessBottom;
                }
            if (clear) patches.push_back({x, y});
        }
    if (patches.empty()) return std::nullopt;
    std::mt19937 random(3);
    auto mirror = [](int v) { return v < 0 ? -v - 1 : v >= N ? 2 * N - v - 1 : v; };
    std::vector<Rgb> out = rgb;
    for (;;) {
        // The hole texel beside known floor with the most known floor round it.
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

    // 3. The O, raised and lit as the B is. Where its edge faces up or left it has the B's dimmer
    // edge there, texel for texel in from the edge; where it faces down or right, the B's bright
    // one, the brightest where it faces the light squarely. Each is copied from the B's own edge,
    // in order round the O, so it runs on as the B's does. The rest is the B's own face: the same
    // texel where the face was, else the nearest. Round it, inside and out, the B's dark outline.
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
    // How many texels in from the stroke's nearer edge each of its texels is.
    std::array<int, N * N> depth{};
    Grid left = o;
    for (int step = 1; std::find(left.begin(), left.end(), true) != left.end(); step++) {
        const Grid next = erode(left);
        for (int i = 0; i < N * N; i++)
            if (left[i] && !next[i]) depth[i] = step;
        left = next;
    }

    // The B's edge texels, by how far in from its edge, and the outline texel just outside: along
    // its stem and across its top, the dim edge; down its bowls and across its bottom, the bright
    // one, darkest first.
    std::array<std::vector<Rgb>, 3> dim, shine;  // [0] is the outline
    auto edge = [&](std::array<std::vector<Rgb>, 3>& to, int x, int y, int dx, int dy) {
        for (int k = 0; k <= 2; k++) to[k].push_back(rgb[at(x + (k - 1) * dx, y + (k - 1) * dy)]);
    };
    for (int y = kTop + 1; y < kBottom; y++)
        for (int x = kLeft; x < 30; x++)
            if (letter[at(x, y)]) {
                edge(dim, x, y, 1, 0);
                break;
            }
    for (int x = 18; x < 38; x++)
        for (int y = kTop - 1; y < 20; y++)
            if (letter[at(x, y)]) {
                edge(dim, x, y, 0, 1);
                break;
            }
    for (int y = 14; y < 50; y++) {
        if (y >= 30 && y < 39) continue;  // where the bowls meet
        for (int x = kRight; x > 30; x--)
            if (letter[at(x, y)]) {
                edge(shine, x, y, -1, 0);
                break;
            }
    }
    for (int x = 18; x < 36; x++)
        for (int y = kBottom; y > 40; y--)
            if (letter[at(x, y)]) {
                edge(shine, x, y, 0, -1);
                break;
            }
    for (int k = 0; k <= 2; k++)
        if (dim[k].empty() || shine[k].empty()) return std::nullopt;
    for (int k = 1; k <= 2; k++)
        std::stable_sort(shine[k].begin(), shine[k].end(), [](const Rgb& a, const Rgb& b) { return lum(a) < lum(b); });
    Grid face_inside = erode(face, 2);
    std::vector<int> core;  // the B's face away from its edges
    for (int i = 0; i < N * N; i++)
        if (face_inside[i]) core.push_back(i);
    if (core.empty()) return std::nullopt;

    const Grid outline = dilate(o);
    const float middle = kOuter - kWidth / 2;
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            const int i = at(x, y);
            if (!outline[i]) continue;
            const float dx = x + 0.5f - kOX, dy = y + 0.5f - kOY, d = std::hypot(dx, dy);
            const float lit = (d > middle ? 1.0f : -1.0f) * (dx + dy);  // its edge facing the lower right
            const int along = int((std::atan2(dy, dx) + float(M_PI)) / (2 * float(M_PI)) * 97);
            const int k = o[i] ? depth[i] : 0;
            if (lit < 0 && k <= 2) {
                out[i] = dim[k][along % dim[k].size()];
            } else if (lit >= 0 && k == 0) {
                out[i] = shine[0][along % shine[0].size()];
            } else if (lit >= 0 && k <= 2) {
                const std::vector<Rgb>& v = shine[k];
                const float squarely = lit / (std::sqrt(2.0f) * d);
                const int pick = int(squarely * (v.size() - 1)) + along % 7 - 3;
                out[i] = v[std::clamp(pick, 0, int(v.size()) - 1)];
            } else if (face_inside[i]) {
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

    Texture texture{hash(resource), N, N, std::vector<uint8_t>(N * N * 4)};
    for (int i = 0; i < N * N; i++) {
        for (int c = 0; c < 3; c++) texture.rgba[i * 4 + c] = uint8_t(std::lround(out[i][c]));
        texture.rgba[i * 4 + 3] = alpha[i];
    }
    return texture;
}

} // namespace conker::pad_icons
