// The round stone B plate (as in scene 19, on an octagonal post in the water) is drawn in four
// quarters, each a 32x64 CI8 texture with its own 256-colour RGBA16 palette, flat resources 308 to
// 311. Each quarter is mapped straight onto a quarter of the plate (32 texels across 200 units one
// way, 64 down 200 the other), some mirrored, so together they make one 64x128 picture of it: stone
// cut by a cross of grooves, a raised ring in the middle, and inside it a B sunk into the stone, a
// flat darker recess with soft edges.
//
// For a PlayStation pad or the keyboard everything inside the ring (the B and the smoother stone round it) is
// filled again from its edges inward, each step copying the 5x9 patch of the floor outside the ring,
// clear of the grooves, that best matches what is already round it, so it is the same stone as the
// floor. Then the mark (a square or an E), in its proportions on the plate and centred in the ring's dark outline, is sunk where the B
// was, in the B's own dark texels and with edges as soft. It is made from the player's ROM when the game starts, so no part
// of Rare's texture is in this repository.
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

constexpr int kQuarters[4] = {308, 309, 310, 311};
constexpr int kW = 32, kH = 64;  // each quarter
constexpr int W = 64, H = 128;   // the whole plate
constexpr int kTexelBytes = kW * kH;
constexpr int kResourceBytes = kTexelBytes + 256 * 2;
constexpr uint32_t kTlutRgba16 = 0x8000;  // G_TT_RGBA16, as RT64 hashes a TLUT draw

// Texels per unit on the plate, across and down the picture.
constexpr float kAcross = 32.0f / 200, kDown = 64.0f / 200;
// The ring's dark outer line: its middle and half its width and height, in texels. Inside kInside
// of it is filled again, and the B's corners where they reach out to kLetter; the floor the fill copies from is outside kFloor of it, and more than
// kGrooveX across and kGrooveY down from the middle of the grooves.
constexpr float kCX = 31.5f, kCY = 63.5f, kRX = 18.0f, kRY = 37.5f;
constexpr float kInside = 0.80f, kLetter = 0.92f, kFloor = 1.25f, kGrooveX = 3.0f, kGrooveY = 5.0f;
// The mark, in texels its middle (by the middle of the ring's dark outline, fitted by least squares,
// then half a texel across, which looked better centred in game), and in units on the plate its
// outer half-width and stroke width.
constexpr float kOX = 31.35f, kOY = 62.9f, kOuter = 72.0f, kWidth = 34.0f;

using Rgb = std::array<float, 3>;
using Grid = std::array<bool, W * H>;

int at(int x, int y) { return y * W + x; }
bool in(int x, int y) { return x >= 0 && y >= 0 && x < W && y < H; }
float lum(const Rgb& c) { return (c[0] + c[1] + c[2]) / 3; }
float ring(int x, int y) { return std::hypot((x + 0.5f - kCX) / kRX, (y + 0.5f - kCY) / kRY); }

// Where a quarter's texel (s, t) is in the plate's picture.
int plate_at(int quarter, int s, int t) {
    switch (quarter) {
        case 0: return at(32 + s, 64 + t);
        case 1: return at(31 - s, 64 + t);
        case 2: return at(63 - s, t);
        default: return at(s, t);
    }
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

std::vector<Texture> plate_pad(Mark::Shape shape) {
    const Mark mark{shape, kOuter, kWidth};
    // The plate as RT64 decodes it.
    std::vector<uint8_t> resources[4];
    std::vector<Rgb> rgb(W * H);
    std::vector<uint8_t> alpha(W * H);
    for (int q = 0; q < 4; q++) {
        resources[q] = menu_art::load_resource(kQuarters[q]);
        const std::vector<uint8_t>& r = resources[q];
        if (r.size() != size_t(kResourceBytes)) return {};
        for (int t = 0; t < kH; t++)
            for (int s = 0; s < kW; s++) {
                const uint8_t index = r[size_t(t * kW + s) ^ ((t & 1) ? 4 : 0)];  // odd rows word-swapped
                const uint16_t v = uint16_t(r[kTexelBytes + index * 2] << 8 | r[kTexelBytes + index * 2 + 1]);
                const int i = plate_at(q, s, t);
                for (int c = 0; c < 3; c++) {
                    const int five = (v >> (11 - 5 * c)) & 31;
                    rgb[i][c] = float(five << 3 | five >> 2);
                }
                alpha[i] = (v & 1) ? 255 : 0;
            }
    }

    // 1. The B: the largest patch of dark texels inside the ring.
    Grid dark{};
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) dark[at(x, y)] = ring(x, y) < kLetter && lum(rgb[at(x, y)]) < 66;
    Grid letter{};
    {
        std::array<int, W * H> part{};
        std::vector<int> largest;
        int next = 0;
        for (int start = 0; start < W * H; start++) {
            if (!dark[start] || part[start]) continue;
            std::vector<int> members{start};
            part[start] = ++next;
            for (size_t k = 0; k < members.size(); k++) {
                const int x = members[k] % W, y = members[k] / W;
                const int n[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
                for (const auto& p : n) {
                    if (in(p[0], p[1]) && dark[at(p[0], p[1])] && !part[at(p[0], p[1])]) {
                        part[at(p[0], p[1])] = next;
                        members.push_back(at(p[0], p[1]));
                    }
                }
            }
            if (members.size() > largest.size()) largest = std::move(members);
        }
        if (largest.size() < 200) return {};
        for (int m : largest) letter[m] = true;
    }
    Grid hole{};
    std::vector<int> core;  // the B away from its edges, the dark the mark is sunk in
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const int n[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
            bool all = letter[at(x, y)];
            for (const auto& p : n) all = all && in(p[0], p[1]) && letter[at(p[0], p[1])];
            // The B's soft edge, a texel round it, where that is still darker than the stone.
            bool edge = letter[at(x, y)];
            for (const auto& p : n) edge = edge || (in(p[0], p[1]) && letter[at(p[0], p[1])] && lum(rgb[at(x, y)]) < 70);
            hole[at(x, y)] = ring(x, y) < kInside || (edge && ring(x, y) < kLetter);
            if (all) core.push_back(at(x, y));
        }
    if (core.empty()) return {};

    // 2. Inside the ring, from the edges inward, each step copying the 5x9 patch of the floor that
    // best matches what is already round it. A texel here is twice as long across the plate as down
    // it, hence the patch's shape.
    constexpr int kHalfX = 2, kHalfY = 4;
    auto floor = [](int x, int y) {
        return ring(x, y) > kFloor && std::abs(x + 0.5f - W / 2) > kGrooveX && std::abs(y + 0.5f - H / 2) > kGrooveY;
    };
    struct Patch {
        int x, y;
    };
    std::vector<Patch> patches;
    for (int y = kHalfY; y < H - kHalfY; y++)
        for (int x = kHalfX; x < W - kHalfX; x++) {
            bool clear = true;
            for (int j = -kHalfY; j <= kHalfY && clear; j++)
                for (int i = -kHalfX; i <= kHalfX && clear; i++)
                    clear = floor(x + i, y + j);
            if (clear) patches.push_back({x, y});
        }
    if (patches.empty()) return {};
    std::mt19937 random(3);
    std::vector<Rgb> out = rgb;
    for (;;) {
        // The hole texel beside known stone with the most known stone round it.
        int best_i = -1, best_known = -1;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                if (!hole[at(x, y)]) continue;
                bool front = false;
                const int n[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
                for (const auto& p : n) front = front || (in(p[0], p[1]) && !hole[at(p[0], p[1])]);
                if (!front) continue;
                int known = 0;
                for (int j = -kHalfY; j <= kHalfY; j++)
                    for (int i = -kHalfX; i <= kHalfX; i++) known += in(x + i, y + j) && !hole[at(x + i, y + j)];
                if (known > best_known) best_known = known, best_i = at(x, y);
            }
        if (best_i < 0) break;
        const int x = best_i % W, y = best_i / W;
        std::vector<std::pair<float, size_t>> scores;
        for (size_t p = 0; p < patches.size(); p++) {
            float d = 0;
            for (int j = -kHalfY; j <= kHalfY; j++)
                for (int i = -kHalfX; i <= kHalfX; i++) {
                    if (!in(x + i, y + j) || hole[at(x + i, y + j)]) continue;
                    const Rgb& a = out[at(x + i, y + j)];
                    const Rgb& b = out[at(patches[p].x + i, patches[p].y + j)];
                    for (int c = 0; c < 3; c++) d += (a[c] - b[c]) * (a[c] - b[c]);
                }
            scores.emplace_back(d, p);
        }
        const size_t choices = std::min<size_t>(3, scores.size());
        std::partial_sort(scores.begin(), scores.begin() + choices, scores.end());
        const Patch& chosen = patches[scores[random() % choices].second];
        for (int j = -kHalfY; j <= kHalfY; j++)
            for (int i = -kHalfX; i <= kHalfX; i++) {
                if (!in(x + i, y + j) || !hole[at(x + i, y + j)]) continue;
                out[at(x + i, y + j)] = out[at(chosen.x + i, chosen.y + j)];
                hole[at(x + i, y + j)] = false;
            }
    }

    // 3. The mark, in its proportions on the plate, sunk as the B was: each texel the B's own dark (the same texel
    // where the B was, else the nearest of it), over the stone by how much of the texel the mark
    // covers, so its edges are as soft as the B's.
    const std::vector<Rgb> stone = out;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            constexpr int kSamples = 6;
            int covered = 0;
            for (int sy = 0; sy < kSamples; sy++)
                for (int sx = 0; sx < kSamples; sx++) {
                    covered += mark.stroke((x + (sx + 0.5f) / kSamples - kOX) / kAcross,
                                           (y + (sy + 0.5f) / kSamples - kOY) / kDown);
                }
            if (covered == 0) continue;
            const float cover = float(covered) / (kSamples * kSamples);
            int from = core[0];
            if (std::find(core.begin(), core.end(), at(x, y)) != core.end()) {
                from = at(x, y);
            } else {
                // Nearest on the plate: a texel is twice as long across as down.
                auto far = [&](int c) {
                    const float dx = float(c % W - x), dy = float(c / W - y) / 2;
                    return dx * dx + dy * dy;
                };
                for (int c : core)
                    if (far(c) < far(from)) from = c;
            }
            for (int c = 0; c < 3; c++) out[at(x, y)][c] = stone[at(x, y)][c] * (1 - cover) + rgb[from][c] * cover;
        }

    std::vector<Texture> textures;
    for (int q = 0; q < 4; q++) {
        Texture t{hash(resources[q]), kW, kH, std::vector<uint8_t>(kW * kH * 4)};
        for (int y = 0; y < kH; y++)
            for (int x = 0; x < kW; x++) {
                const int i = plate_at(q, x, y);
                uint8_t* p = &t.rgba[(y * kW + x) * 4];
                for (int c = 0; c < 3; c++) p[c] = uint8_t(std::lround(out[i][c]));
                p[3] = alpha[i];
            }
        textures.push_back(std::move(t));
    }
    return textures;
}

} // namespace conker::pad_icons
