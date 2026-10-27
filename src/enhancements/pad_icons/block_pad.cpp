// The stone block B pads (as in scene 28, Saving Private Rodent) are a 64x64 CI4 texture with a
// 16-colour RGBA16 palette, flat resource 2651: the face of a block that is itself a square B, its
// two counters carved into it as rectangles, one above the other, their right and bottom walls in
// shadow, a thin shadow under their top edge and their left wall lit. The face shades from brown
// at the top to a pale blue-grey at the bottom.
//
// For a PlayStation pad or the keyboard the counters' walls are taken out (their floors are the face
// carrying on) and filled from the texels beside them. Then the mark's counters are carved the same way, their floors a shade darker
// than the face: one rounded square for the Square (the block its outline, as the block is the
// B), or for the E the two gaps between its bars, open to the block's right edge. It is made from
// the player's ROM when the game starts, so no part of Rare's texture is in this repository.
#include "enhancements/pad_icons/b_pad.hpp"
#include "enhancements/pad_icons/pad_mark.hpp"

#include <algorithm>
#include <array>
#include <random>

#include "hle/rt64_application.h"
#include "common/rt64_load_types.h"
#include "common/rt64_tmem_hasher.h"
#include "enhancements/sharp_menus/menu_art.hpp"

namespace conker::pad_icons {

namespace {

constexpr int kResource = 2651;
constexpr int N = 64;
constexpr int kTexelBytes = N * N / 2;
constexpr uint32_t kTlutRgba16 = 0x8000;  // G_TT_RGBA16, as RT64 hashes a TLUT draw

// The B's counters with their walls (inclusive): upper, then lower.
struct Box {
    int left, top, right, bottom;
};
constexpr Box kCounters[2] = {{12, 11, 53, 29}, {14, 38, 52, 55}};
// The Square's counter: its middle and half-width. The E's gaps are the B's counters' rows, carved
// from its stem (kEStem) to the block's right edge but for its middle bar, which ends at kEMiddle.
constexpr float kOX = 32.0f, kOY = 33.0f, kRadius = 20.5f;
constexpr int kEStem = 15, kEMiddle = 48;
// The walls' shades: palette entries, outermost first.
constexpr int kRightWall[] = {2, 1, 1, 1};
constexpr int kBottomWall[] = {1, 0, 1};
constexpr int kTopShadow[] = {2, 3};
constexpr int kLitWall = 7;  // the left wall, facing the light
constexpr float kFloor = 0.9f;
constexpr int kDarkest = 3;  // the palette's entries up to this are the walls' shades
constexpr int kLitEdge = 1;  // texels in from a counter's left edge that are its lit wall  // how bright a counter's floor is, of the face

using Grid = std::array<bool, N * N>;

int at(int x, int y) { return y * N + x; }
bool in(int x, int y) { return x >= 0 && y >= 0 && x < N && y < N; }

// What of the block the mark's counters take out.
Grid carved(Mark::Shape shape) {
    Grid g{};
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            if (shape == Mark::Shape::Square) {
                // Sampled at four points: carved if most of the texel is in the counter.
                int inside = 0;
                for (int s = 0; s < 4; s++)
                    inside += square_symbol::reach(x + 0.25f + (s & 1) * 0.5f - kOX, y + 0.25f + (s >> 1) * 0.5f - kOY) < kRadius;
                g[at(x, y)] = inside >= 2;
            } else {
                const Box& upper = kCounters[0];
                const Box& lower = kCounters[1];
                const bool gap = x >= kEStem && ((y > upper.top && y < upper.bottom) || (y > lower.top && y < lower.bottom));
                const bool past_middle = x >= kEMiddle && y > upper.top && y < lower.bottom;
                g[at(x, y)] = gap || past_middle;
            }
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

std::optional<Texture> block_pad(Mark::Shape shape) {
    const std::vector<uint8_t> resource = menu_art::load_resource(kResource);
    if (resource.size() != size_t(kTexelBytes + 32)) return std::nullopt;

    // The palette as RT64 decodes RGBA16, and each texel's entry (odd rows are word-swapped).
    std::array<std::array<float, 3>, 16> palette;
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

    // 1. The B taken out: its counters' floors are the face carrying on, so only their walls go,
    // the dark texels in the counters and the lit edge down their left side, filled from their
    // edges inward, each texel from a known texel beside it.
    Grid hole{};
    for (const Box& b : kCounters)
        for (int y = b.top; y <= b.bottom; y++)
            for (int x = b.left; x <= b.right; x++) hole[at(x, y)] = index[at(x, y)] <= kDarkest || x <= b.left + kLitEdge;
    std::mt19937 random(3);
    std::array<int, N * N> out = index;
    for (;;) {
        std::vector<std::pair<int, int>> front;  // a hole texel and a known one beside it
        for (int y = 0; y < N; y++)
            for (int x = 0; x < N; x++) {
                if (!hole[at(x, y)]) continue;
                std::vector<int> known;
                const int n[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
                for (const auto& q : n)
                    if (in(q[0], q[1]) && !hole[at(q[0], q[1])]) known.push_back(at(q[0], q[1]));
                if (!known.empty()) front.emplace_back(at(x, y), known[random() % known.size()]);
            }
        if (front.empty()) break;
        for (const auto& [t, from] : front) out[t] = out[from], hole[t] = false;
    }

    // 2. The mark's counters carved as the B's are: their right and bottom walls in shadow, a thin
    // shadow under their top edge and their left wall lit; and their floor the face a shade darker (Rare's counters
    // are closed all round, the E's gaps open to the block's edge, so the shade is what shows them).
    std::array<int, 16> darker;
    for (int i = 0; i < 16; i++) {
        float best = 1e30f;
        darker[i] = i;
        for (int j = 0; j < 16; j++) {
            float d = 0;
            for (int c = 0; c < 3; c++) d += (palette[j][c] - palette[i][c] * kFloor) * (palette[j][c] - palette[i][c] * kFloor);
            if (opaque[j] && d < best) best = d, darker[i] = j;
        }
    }
    const Grid cut = carved(shape);
    auto solid = [&](int x, int y) { return in(x, y) && !cut[at(x, y)]; };
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            if (!cut[at(x, y)]) continue;
            int shade = solid(x - 1, y) ? kLitWall : -1;
            for (int k = 0; k < int(std::size(kTopShadow)); k++)
                if (solid(x, y - 1 - k)) {
                    shade = kTopShadow[k];
                    break;
                }
            for (int k = 0; k < int(std::size(kBottomWall)); k++)
                if (solid(x, y + 1 + k)) {
                    shade = kBottomWall[std::size(kBottomWall) - 1 - k];
                    break;
                }
            for (int k = 0; k < int(std::size(kRightWall)); k++)
                if (solid(x + 1 + k, y)) {
                    shade = kRightWall[std::size(kRightWall) - 1 - k];
                    break;
                }
            out[at(x, y)] = shade >= 0 ? shade : darker[out[at(x, y)]];
        }

    Texture texture{hash(resource), N, N, std::vector<uint8_t>(N * N * 4)};
    for (int i = 0; i < N * N; i++) {
        for (int c = 0; c < 3; c++) texture.rgba[i * 4 + c] = uint8_t(palette[out[i]][c]);
        texture.rgba[i * 4 + 3] = opaque[out[i]] ? 255 : 0;
    }
    return texture;
}

} // namespace conker::pad_icons
