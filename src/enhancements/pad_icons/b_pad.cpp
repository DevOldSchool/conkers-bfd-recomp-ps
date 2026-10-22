// The B pads are a 64x64 CI4 texture with a 16-colour RGBA16 palette, flat resource 1575: a
// wooden octagon with a raised B painted in five shades of moss. For a PlayStation pad the B is
// taken out, the wood under it refilled from the same ring of the plate elsewhere (its grain runs
// in octagonal rings), and a circle painted in, in Rare's own colours: the moss mottled the way
// theirs is, lightest on the edges facing up and right, with a dark band two texels wide in the
// palette's dark browns where the B has one: its outline moved up and to the left (outside the top
// and left of the letter, inside its bowls at their lower right). It is made from the player's ROM
// when the game starts, so no part of Rare's texture is in this repository.
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

constexpr int kResource = 1575;
constexpr int kSize = 64;
constexpr int kTexelBytes = kSize * kSize / 2;
constexpr uint32_t kTlutRgba16 = 0x8000;  // G_TT_RGBA16, as RT64 hashes a TLUT draw

using Grid = std::array<bool, kSize * kSize>;
using Field = std::array<float, kSize * kSize>;
using Rgb = std::array<float, 3>;

int at(int x, int y) { return y * kSize + x; }
bool in(int x, int y) { return x >= 0 && y >= 0 && x < kSize && y < kSize; }

// The four-neighbour dilation and erosion scipy.ndimage uses by default.
Grid dilate(const Grid& g, int times = 1) {
    Grid out = g;
    for (int t = 0; t < times; t++) {
        const Grid prev = out;
        for (int y = 0; y < kSize; y++) {
            for (int x = 0; x < kSize; x++) {
                bool v = prev[at(x, y)];
                const int n[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
                for (const auto& p : n) v = v || (in(p[0], p[1]) && prev[at(p[0], p[1])]);
                out[at(x, y)] = v;
            }
        }
    }
    return out;
}

Grid erode(const Grid& g) {
    Grid inverse;
    for (size_t i = 0; i < g.size(); i++) inverse[i] = !g[i];
    const Grid grown = dilate(inverse);
    Grid out;
    for (size_t i = 0; i < g.size(); i++) out[i] = !grown[i];
    return out;
}

// Only the parts of `g` larger than `size` pixels (four-connected).
Grid large_parts(const Grid& g, int size) {
    Grid out{};
    std::array<int, kSize * kSize> part{};
    int next = 0;
    for (int start = 0; start < kSize * kSize; start++) {
        if (!g[start] || part[start]) continue;
        std::vector<int> members{start};
        part[start] = ++next;
        for (size_t k = 0; k < members.size(); k++) {
            const int x = members[k] % kSize, y = members[k] / kSize;
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

Field blur(const Field& f, float sigma) {
    const int radius = int(std::ceil(sigma * 4));
    std::vector<float> kernel(2 * radius + 1);
    float sum = 0;
    for (int i = -radius; i <= radius; i++) sum += kernel[i + radius] = std::exp(-0.5f * i * i / (sigma * sigma));
    for (float& k : kernel) k /= sum;
    auto mirror = [](int v) { return v < 0 ? -v - 1 : v >= kSize ? 2 * kSize - v - 1 : v; };
    Field across{}, out{};
    for (int y = 0; y < kSize; y++)
        for (int x = 0; x < kSize; x++)
            for (int i = -radius; i <= radius; i++) across[at(x, y)] += f[at(mirror(x + i), y)] * kernel[i + radius];
    for (int y = 0; y < kSize; y++)
        for (int x = 0; x < kSize; x++)
            for (int i = -radius; i <= radius; i++) out[at(x, y)] += across[at(x, mirror(y + i))] * kernel[i + radius];
    return out;
}

// The circle, as much of each texel as lies in the ring, kept where that is most of it.
Grid circle() {
    constexpr float kOuter = kSize * 0.27f, kInner = kOuter - kSize * 0.13f;
    Grid out{};
    for (int y = 0; y < kSize; y++) {
        for (int x = 0; x < kSize; x++) {
            int inside = 0;
            for (int sy = 0; sy < 8; sy++) {
                for (int sx = 0; sx < 8; sx++) {
                    const float dx = x + (sx + 0.5f) / 8 - kSize / 2.0f, dy = y + (sy + 0.5f) / 8 - kSize / 2.0f;
                    const float d = std::sqrt(dx * dx + dy * dy);
                    inside += d >= kInner && d <= kOuter;
                }
            }
            out[at(x, y)] = inside > 64 * 0.43f;
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
    load.line = kSize / 2 / 8;
    return RT64::TMEMHasher::hash(tmem.data(), load, kSize, kSize, kTlutRgba16, RT64::TMEMHasher::CurrentHashVersion);
}

} // namespace

std::optional<Texture> circle_pad() {
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
    std::array<int, kSize * kSize> index;
    for (int y = 0; y < kSize; y++) {
        for (int x = 0; x < kSize; x++) {
            const int byte = (y * kSize + x) / 2 ^ ((y & 1) ? 4 : 0);
            index[at(x, y)] = (x & 1) ? resource[byte] & 15 : resource[byte] >> 4;
        }
    }

    // The moss shades, lightest first, and the dark browns of its shadow.
    std::vector<int> moss, browns;
    for (int i = 0; i < 16; i++) {
        const Rgb& c = palette[i];
        const float brightest = std::max({c[0], c[1], c[2]});
        if (c[1] > 0.8f * c[0] && c[0] > 100) moss.push_back(i);
        else if (brightest > 10 && brightest < 70) browns.push_back(i);
    }
    std::sort(moss.begin(), moss.end(), [&](int a, int b) {
        return palette[a][0] + palette[a][1] + palette[a][2] > palette[b][0] + palette[b][1] + palette[b][2];
    });
    if (moss.size() != 5 || browns.empty()) return std::nullopt;

    Grid paint{}, solid{};
    for (int i = 0; i < kSize * kSize; i++) {
        paint[i] = std::find(moss.begin(), moss.end(), index[i]) != moss.end();
        solid[i] = opaque[index[i]];
    }
    paint = large_parts(paint, 30);  // the letter, not moss-coloured specks in the wood
    const Grid near = dilate(paint, 3);
    Grid letter{};
    for (int i = 0; i < kSize * kSize; i++) {
        const Rgb& c = palette[index[i]];
        letter[i] = paint[i] || (near[i] && std::max({c[0], c[1], c[2]}) < 70);
    }
    letter = erode(dilate(letter));

    // Refill the wood under the letter from the same ring, at the nearest angles.
    std::mt19937 random(3);
    auto ring = [](int x, int y) {
        const float dx = std::abs(x + 0.5f - kSize / 2.0f), dy = std::abs(y + 0.5f - kSize / 2.0f);
        return int(std::lround(2 * std::max({dx, dy, (dx + dy) / 1.41421356f})));
    };
    auto angle = [](int x, int y) { return std::atan2(y + 0.5f - kSize / 2.0f, x + 0.5f - kSize / 2.0f); };
    const Grid covered = dilate(letter);
    std::array<int, kSize * kSize> wood = index;
    for (int y = 0; y < kSize; y++) {
        for (int x = 0; x < kSize; x++) {
            if (!covered[at(x, y)] || !solid[at(x, y)]) continue;
            std::vector<std::pair<float, int>> candidates;
            for (int reach : {0, 1, 2, 4, 8, 16, 64}) {
                for (int cy = 0; cy < kSize; cy++) {
                    for (int cx = 0; cx < kSize; cx++) {
                        if (covered[at(cx, cy)] || !solid[at(cx, cy)] || std::abs(ring(cx, cy) - ring(x, y)) > reach) continue;
                        const float turn = std::remainder(angle(cx, cy) - angle(x, y), 2 * float(M_PI));
                        candidates.emplace_back(std::abs(turn), index[at(cx, cy)]);
                    }
                }
                if (!candidates.empty()) break;
            }
            if (candidates.empty()) continue;
            std::sort(candidates.begin(), candidates.end());
            const size_t pick = std::min(candidates.size() - 1, size_t(random() % 8));
            wood[at(x, y)] = candidates[pick].second;
        }
    }

    // The circle, with the B's dark band: the circle moved one and two texels up and left.
    const Grid ring_mask = circle();
    std::array<int, kSize * kSize> out = wood;
    for (int y = 0; y < kSize; y++) {
        for (int x = 0; x < kSize; x++) {
            const bool shadow = !ring_mask[at(x, y)] && solid[at(x, y)] &&
                                ((in(x + 1, y + 1) && ring_mask[at(x + 1, y + 1)]) || (in(x + 2, y + 2) && ring_mask[at(x + 2, y + 2)]));
            if (shadow) out[at(x, y)] = browns[random() % browns.size()];
        }
    }
    Field noise;
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    for (float& v : noise) v = unit(random);
    noise = blur(noise, 0.9f);
    std::vector<float> inside;
    for (int i = 0; i < kSize * kSize; i++)
        if (ring_mask[i]) inside.push_back(noise[i]);
    std::sort(inside.begin(), inside.end());
    const float cuts[3] = {inside[inside.size() * 20 / 100], inside[inside.size() * 55 / 100], inside[inside.size() * 85 / 100]};
    auto on = [&](int x, int y) { return in(x, y) && ring_mask[at(x, y)]; };
    for (int y = 0; y < kSize; y++) {
        for (int x = 0; x < kSize; x++) {
            if (!ring_mask[at(x, y)]) continue;
            const float v = noise[at(x, y)];
            int shade = 1 + (v >= cuts[0]) + (v >= cuts[1]) + (v >= cuts[2]);
            if (!on(x - 1, y) || !on(x, y + 1)) shade = std::max(shade, 3);  // edges facing down and left
            if (!on(x, y - 1)) shade = 1;                                    // facing up
            if (!on(x + 1, y)) shade = 0;                                    // facing right, into the light
            out[at(x, y)] = moss[shade];
        }
    }

    Texture texture{hash(resource), kSize, kSize, std::vector<uint8_t>(kSize * kSize * 4)};
    for (int i = 0; i < kSize * kSize; i++) {
        for (int c = 0; c < 3; c++) texture.rgba[i * 4 + c] = uint8_t(palette[out[i]][c]);
        texture.rgba[i * 4 + 3] = opaque[out[i]] ? 255 : 0;
    }
    return texture;
}

} // namespace conker::pad_icons
