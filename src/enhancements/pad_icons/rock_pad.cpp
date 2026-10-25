// The grey stone B pads (as in scene 49, Rock Solid) are a 64x64 I8 texture, flat resource 93,
// tinted by the floor's colour: a stone frame round a dark square, and in it a big raised B with a
// rounded edge, lit from the lower right. Each edge of the B slopes up to a bright crest and then
// its flat face: on its lower and right edges the crest is at the edge, on its upper and left ones
// a few texels in, past a dim slope. It casts its shadow up and to the left, a dark band above it
// and left of it, and its counters are dark.
//
// For a PlayStation pad the B and its shadow are taken out and filled from their edges inward,
// each step copying the 3x3 patch of the lit floor beside and below the B that best matches what is
// already round it. Then a square stands where the B was, as bold as the B: across each of its edges
// the brightness of the B's edges facing the same way, measured from their crests (so the crest
// stays a sharp line at the depth the B has it facing that way), with the mottling of the B's
// face over it, and its shadow cast up and to the left as the B's is. It is made from the
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

constexpr int kResource = 93;
constexpr int N = 64;
constexpr int kTexelBytes = N * N;

// The B stands in x 15-50, y 8-55 (inclusive); its stem is at (18, 30). Its shadow falls so many
// texels left and up. The lit floor the B and its shadow are filled from: right of the B and
// below it. The square: its middle, outer half-width and stroke width, in texels.
constexpr int kLeft = 15, kTop = 8, kRight = 50, kBottom = 55, kStemX = 18, kStemY = 30;
constexpr float kShadowX = -8, kShadowY = -5;
constexpr float kOX = 33.0f, kOY = 32.0f, kOuter = 17.0f, kWidth = 9.5f;
constexpr float kLetter = 30;      // the least brightness of the B (its dim slope)
constexpr float kHighlight = 100;  // the least brightness of its crest along its right side
constexpr int kDepths = 8;         // how far into the B its edges are measured
constexpr float kSpread = 25.0f * float(M_PI) / 180;  // how widely edges facing nearly the same way are mixed
constexpr float kGrain = 1.0f;     // how strong the B's mottling is on the square

using Grid = std::array<bool, N * N>;
using Image = std::array<float, N * N>;

int at(int x, int y) { return y * N + x; }
bool in(int x, int y) { return x >= 0 && y >= 0 && x < N && y < N; }

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
    // Outside the texture counts as not in g, as scipy's erosion has it.
    Grid grown = inverse;
    for (int t = 0; t < times; t++) {
        grown = dilate(grown);
        for (int i = 0; i < N; i++) grown[at(i, 0)] = grown[at(i, N - 1)] = grown[at(0, i)] = grown[at(N - 1, i)] = true;
    }
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

// `g` with its holes (what of the rest the texture's edge cannot reach) filled.
Grid fill_holes(const Grid& g) {
    Grid outside{};
    std::vector<int> members;
    for (int i = 0; i < N; i++)
        for (int e : {at(i, 0), at(i, N - 1), at(0, i), at(N - 1, i)})
            if (!g[e] && !outside[e]) outside[e] = true, members.push_back(e);
    for (size_t k = 0; k < members.size(); k++) {
        const int mx = members[k] % N, my = members[k] / N;
        const int n[4][2] = {{mx - 1, my}, {mx + 1, my}, {mx, my - 1}, {mx, my + 1}};
        for (const auto& p : n) {
            if (in(p[0], p[1]) && !g[at(p[0], p[1])] && !outside[at(p[0], p[1])]) {
                outside[at(p[0], p[1])] = true;
                members.push_back(at(p[0], p[1]));
            }
        }
    }
    Grid out;
    for (int i = 0; i < N * N; i++) out[i] = !outside[i];
    return out;
}

// A Gaussian blur, edges mirrored, as scipy's gaussian_filter.
Image blur(const Image& image, float sigma) {
    const int radius = int(4 * sigma + 0.5f);
    std::vector<float> kernel(2 * radius + 1);
    float total = 0;
    for (int k = -radius; k <= radius; k++) total += kernel[k + radius] = std::exp(-0.5f * k * k / (sigma * sigma));
    for (float& k : kernel) k /= total;
    auto mirror = [](int i) { return i < 0 ? -i - 1 : i >= N ? 2 * N - i - 1 : i; };
    Image across{}, out{};
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++)
            for (int k = -radius; k <= radius; k++) across[at(x, y)] += kernel[k + radius] * image[at(mirror(x + k), y)];
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++)
            for (int k = -radius; k <= radius; k++) out[at(x, y)] += kernel[k + radius] * across[at(x, mirror(y + k))];
    return out;
}

float median(std::vector<float> values) {
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    const size_t m = values.size() / 2;
    return values.size() % 2 ? values[m] : (values[m - 1] + values[m]) / 2;
}

// How much of each texel a ring round (cx, cy) covers, from 8x8 samples.
Image ring(float cx, float cy) {
    Image cover{};
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            int inside = 0;
            for (int sy = 0; sy < 8; sy++)
                for (int sx = 0; sx < 8; sx++) {
                    const float d = square_symbol::reach(x + (sx + 0.5f) / 8 - cx, y + (sy + 0.5f) / 8 - cy);
                    inside += d >= kOuter - kWidth && d <= kOuter;
                }
            cover[at(x, y)] = inside / 64.0f;
        }
    return cover;
}

uint64_t hash(const std::vector<uint8_t>& resource) {
    std::vector<uint8_t> tmem(4096, 0);
    std::copy_n(resource.begin(), kTexelBytes, tmem.begin());
    RT64::LoadTile load{};
    load.fmt = 4;  // G_IM_FMT_I
    load.siz = 1;  // G_IM_SIZ_8b
    load.line = N / 8;
    return RT64::TMEMHasher::hash(tmem.data(), load, N, N, 0, RT64::TMEMHasher::CurrentHashVersion);
}

// A line of brightness across an edge of the B, from its edge texel inward: the way the edge
// faces and how deep its crest is.
struct Profile {
    float facing;
    int crest;
    std::array<float, kDepths> brightness;
};

// The brightness of an edge facing `facing`, `depth` texels in: the B's edges facing about the
// same way, each read so far from its own crest as this edge's crest (their mean depth) is from
// `depth`. Negative if there are none.
float across(const std::vector<Profile>& profiles, float facing, float depth) {
    float crest = 0, weight = 0;
    std::vector<float> weights(profiles.size());
    for (size_t i = 0; i < profiles.size(); i++) {
        const float turn = std::remainder(profiles[i].facing - facing, 2 * float(M_PI));
        weights[i] = std::exp(-0.5f * (turn / kSpread) * (turn / kSpread));
        crest += weights[i] * float(profiles[i].crest);
        weight += weights[i];
    }
    if (weight <= 0) return -1;
    crest /= weight;
    float sum = 0, used = 0;
    for (size_t i = 0; i < profiles.size(); i++) {
        const float k = float(profiles[i].crest) + depth - crest;
        if (weights[i] < 1e-3f || k < 0 || k > kDepths - 1) continue;
        const int k0 = int(k);
        const float f = k - float(k0);
        const auto& b = profiles[i].brightness;
        sum += weights[i] * (b[k0] * (1 - f) + b[std::min(k0 + 1, kDepths - 1)] * f);
        used += weights[i];
    }
    return used > 0 ? sum / used : -1;
}

} // namespace

std::optional<Texture> rock_pad() {
    const std::vector<uint8_t> resource = menu_art::load_resource(kResource);
    if (resource.size() != size_t(kTexelBytes)) return std::nullopt;

    // The tile as RT64 decodes it (odd rows are word-swapped).
    Image a;
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) a[at(x, y)] = resource[(y * N + x) ^ ((y & 1) ? 4 : 0)];
    auto bilinear = [&](float x, float y) {
        const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
        const float fx = x - float(x0), fy = y - float(y0);
        auto texel = [&](int tx, int ty) { return a[at(std::clamp(tx, 0, N - 1), std::clamp(ty, 0, N - 1))]; };
        return (texel(x0, y0) * (1 - fx) + texel(x0 + 1, y0) * fx) * (1 - fy) +
               (texel(x0, y0 + 1) * (1 - fx) + texel(x0 + 1, y0 + 1) * fx) * fy;
    };

    // 1. The B: what is not dark in its box, up to the crest along its right side (the lit floor
    // right of it is as bright as its face), joined to its stem and closed over the odd dark texel.
    std::array<int, N> right{};
    for (int y = kTop; y <= kBottom; y++) {
        right[y] = -1;
        for (int x = 40; x <= kRight + 1; x++)
            if (a[at(x, y)] >= kHighlight) right[y] = x;
        if (right[y] < 0) {
            int x = 40;
            while (x < kRight + 1 && a[at(x + 1, y)] >= kLetter) x++;
            right[y] = x;
        }
    }
    Grid bright{};
    for (int y = kTop; y <= kBottom; y++)
        for (int x = kLeft; x <= kRight; x++) bright[at(x, y)] = a[at(x, y)] >= kLetter && x <= right[y];
    const Grid face = part_at(bright, kStemX, kStemY);
    Grid letter = erode(dilate(face));
    for (int i = 0; i < N * N; i++) letter[i] = letter[i] || face[i];
    const Grid filled = fill_holes(letter);  // with its counters

    // 2. The B and its shadow taken out. Its shadow is the dark a step toward the light from
    // which reaches the B. Filled from the edges inward, each step copying the 3x3 patch of the
    // lit floor that best matches the known texels round it.
    Grid shadow{};
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++)
            for (int step = 1; step <= 20 && !shadow[at(x, y)]; step++) {
                const float t = 0.1f + 0.9f * float(step - 1) / 19;
                const int sx = int(std::lround(x - kShadowX * t)), sy = int(std::lround(y - kShadowY * t));
                shadow[at(x, y)] = in(sx, sy) && filled[at(sx, sy)];
            }
    Grid hole;
    for (int i = 0; i < N * N; i++) hole[i] = filled[i] || (shadow[i] && a[i] < 40);
    hole = dilate(hole);
    for (int i = 0; i < N; i++) hole[at(i, 0)] = hole[at(i, N - 1)] = hole[at(0, i)] = hole[at(N - 1, i)] = false;
    const Grid away = dilate(hole, 3);
    Grid floor{};
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++)
            floor[at(x, y)] = !away[at(x, y)] && ((y >= 9 && y <= 54 && x >= 51 && x <= 56) || (y >= 56 && y <= 59 && x >= 12 && x <= 45));
    constexpr int kHalf = 1;
    std::vector<int> patches;  // their middles
    for (int y = kHalf; y < N - 1 - kHalf; y++)
        for (int x = kHalf; x < N - 1 - kHalf; x++) {
            bool all = true;
            for (int j = -kHalf; j <= kHalf; j++)
                for (int i = -kHalf; i <= kHalf; i++) all = all && floor[at(x + i, y + j)];
            if (all) patches.push_back(at(x, y));
        }
    if (patches.size() < 2) return std::nullopt;
    std::mt19937 random(3);
    Image out = a;
    Grid known;
    for (int i = 0; i < N * N; i++) known[i] = !hole[i];
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
                for (int j = -kHalf; j <= kHalf; j++)
                    for (int i = -kHalf; i <= kHalf; i++) count += in(x + i, y + j) && known[at(x + i, y + j)];
                if (count > best_known) best_known = count, best_i = at(x, y);
            }
        if (best_i < 0) break;
        const int x = best_i % N, y = best_i / N;
        std::vector<std::pair<float, int>> scores;
        for (int p : patches) {
            float d = 0;
            for (int j = -kHalf; j <= kHalf; j++)
                for (int i = -kHalf; i <= kHalf; i++) {
                    if (!in(x + i, y + j) || !known[at(x + i, y + j)]) continue;
                    const float e = out[at(x + i, y + j)] - a[p + j * N + i];
                    d += e * e;
                }
            scores.emplace_back(d, p);
        }
        std::partial_sort(scores.begin(), scores.begin() + 2, scores.end());
        const int chosen = scores[random() % 2].second;
        for (int j = -kHalf; j <= kHalf; j++)
            for (int i = -kHalf; i <= kHalf; i++) {
                const int t = at(x + i, y + j);
                if (!in(x + i, y + j) || known[t]) continue;
                out[t] = a[chosen + j * N + i];
                known[t] = true;
            }
    }

    // 3. The square's shadow: the floor darkened as the B's shadow darkens it, by how much of the
    // texel the square covers moved toward where its shadow falls (so its edge is soft).
    std::vector<float> dark, lit;
    for (int i = 0; i < N * N; i++) {
        if (shadow[i] && !filled[i] && a[i] < 20) dark.push_back(a[i]);
        if (!away[i]) lit.push_back(a[i]);
    }
    const float darkening = median(dark) / std::max(median(lit), 1.0f);
    Image cast{};
    for (int step = 1; step <= 16; step++) {
        const float t = step / 16.0f;
        const Image moved = ring(kOX + kShadowX * t, kOY + kShadowY * t);
        for (int i = 0; i < N * N; i++) cast[i] = std::max(cast[i], moved[i]);
    }
    for (int i = 0; i < N * N; i++) out[i] *= 1 - (1 - darkening) * cast[i];

    // 4. The B's edges: from each edge texel inward, square to the edge (its direction from a
    // blur of the B), outer edges and its counters' apart.
    Image soft;
    for (int i = 0; i < N * N; i++) soft[i] = letter[i] ? 1.0f : 0.0f;
    soft = blur(soft, 1.2f);
    const Grid inner = erode(letter);
    Grid counter;
    for (int i = 0; i < N * N; i++) counter[i] = filled[i] && !letter[i];
    const Grid by_counter = dilate(counter);
    std::vector<Profile> outer_edges, counter_edges;
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            if (!letter[at(x, y)] || inner[at(x, y)]) continue;
            // np.gradient: central differences, one-sided at the texture's edge.
            const float gx = x == 0 ? soft[at(1, y)] - soft[at(0, y)]
                             : x == N - 1 ? soft[at(x, y)] - soft[at(x - 1, y)]
                                          : (soft[at(x + 1, y)] - soft[at(x - 1, y)]) / 2;
            const float gy = y == 0 ? soft[at(x, 1)] - soft[at(x, 0)]
                             : y == N - 1 ? soft[at(x, y)] - soft[at(x, y - 1)]
                                          : (soft[at(x, y + 1)] - soft[at(x, y - 1)]) / 2;
            const float length = std::hypot(gx, gy);
            if (length < 1e-6f) continue;
            const float nx = gx / length, ny = gy / length;  // into the B
            Profile profile{std::atan2(-ny, -nx), 0, {}};
            profile.brightness[0] = a[at(x, y)];
            for (int k = 1; k < kDepths; k++) profile.brightness[k] = bilinear(x + nx * k, y + ny * k);
            profile.crest = int(std::max_element(profile.brightness.begin(), profile.brightness.begin() + 6) -
                                profile.brightness.begin());
            (by_counter[at(x, y)] ? counter_edges : outer_edges).push_back(profile);
        }
    if (outer_edges.empty() || counter_edges.empty()) return std::nullopt;

    // 5. The B's grain: its mottling (its face less a wide blur of it), from the middle of its
    // face clear of its crests, each texel from the first of a few places round it that is there,
    // so it is the B's own mottling everywhere.
    const Image smooth = blur(a, 3.0f);
    const Grid middle_face = erode(letter, 5);
    constexpr int kFrom[][2] = {{0, 0}, {7, 9}, {-7, 9}, {7, -9}, {-7, -9}, {12, 3},
                                {-12, 3}, {3, 15}, {-3, -15}, {12, -12}, {-12, 12}};
    Image grain{};
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++)
            for (const auto& from : kFrom) {
                const int sx = x + from[0], sy = y + from[1];
                if (!in(sx, sy) || !middle_face[at(sx, sy)]) continue;
                grain[at(x, y)] = std::clamp(a[at(sx, sy)] - smooth[at(sx, sy)], -25.0f, 25.0f) * kGrain;
                break;
            }

    // 6. The square: across its outer edge the B's outer edges facing the same way, across its inner
    // edge the B's counters', blended over the middle of its stroke.
    const Image o = ring(kOX, kOY);
    const float middle = kOuter - kWidth / 2;
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            const int i = at(x, y);
            if (o[i] <= 0.45f) continue;
            const float dx = x + 0.5f - kOX, dy = y + 0.5f - kOY, r = square_symbol::reach(dx, dy);
            const float turn = square_symbol::turn(dx, dy);
            // A B edge texel's middle is half a texel in from its edge.
            const float outside = across(outer_edges, turn, std::max(kOuter - r - 0.5f, 0.0f));
            const float inside = across(counter_edges, turn + float(M_PI), std::max(r - (kOuter - kWidth) - 0.5f, 0.0f));
            float t = std::clamp((r - middle) / 3 + 0.5f, 0.0f, 1.0f);  // 1 toward the outer edge
            if (outside < 0) t = 0;
            if (inside < 0) t = 1;
            out[i] = t * std::max(outside, 0.0f) + (1 - t) * std::max(inside, 0.0f) + grain[i];
        }

    // An I texture's texels are its colour and its alpha alike.
    Texture texture{hash(resource), N, N, std::vector<uint8_t>(N * N * 4)};
    for (int i = 0; i < N * N; i++) {
        const uint8_t v = uint8_t(std::clamp(std::lround(out[i]), 0L, 255L));
        for (int c = 0; c < 4; c++) texture.rgba[i * 4 + c] = v;
    }
    return texture;
}

} // namespace conker::pad_icons
