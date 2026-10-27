// The purple disc B pads (as in scene 28, Saving Private Rodent) are a dark purple disc on grey,
// split into two 32x64 CI8 textures with their own 256-colour RGBA16 palettes, flat resources
// 3672 (left) and 3673 (right). A fat paler purple B, leaning a little and its counters small
// dots, stands in the middle of it, and a sheen runs across the whole disc, B and all, in
// horizontal streaks.
//
// For a PlayStation pad or the keyboard the B and its counters are taken out and each row of them
// filled from the disc beside it in the same row, mirrored outward from the B's edge (and round
// again where the disc there is narrower than the B), so the streaks run straight through. Then
// the mark (a square, or an E for the keyboard), as fat as the B and leaning as it does, is laid
// where the B was in the B's own purple: the same texel where the B's face was, else the nearest
// in the same row so its streaks carry on, blended into the disc at its edges. It is made from the
// player's ROM when the game starts, so no part of Rare's texture is in this repository.
#include "enhancements/pad_icons/b_pad.hpp"
#include "enhancements/pad_icons/pad_mark.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#include "hle/rt64_application.h"
#include "common/rt64_load_types.h"
#include "common/rt64_tmem_hasher.h"
#include "enhancements/sharp_menus/menu_art.hpp"

namespace conker::pad_icons {

namespace {

constexpr int kHalves[2] = {3672, 3673};  // left, right
constexpr int kW = 32, kH = 64;           // each half
constexpr int N = 64;                     // the whole disc
constexpr int kTexelBytes = kW * kH;
constexpr int kResourceBytes = kTexelBytes + 256 * 2;
constexpr uint32_t kTlutRgba16 = 0x8000;  // G_TT_RGBA16, as RT64 hashes a TLUT draw

// The box the B stands in (inclusive), a texel of its stem, and how bright (r + g + b) its face
// is at the least; the disc's middle and radius. Then the mark, where the B is and as heavy as it
// (Rare's B is fat, its counters small dots), leaning as it does: its middle, how far it leans
// (texels right for each texel up), and for the square and the E its outer half-width (the E's
// half height) and stroke width, in texels.
constexpr int kLeft = 13, kTop = 11, kRight = 48, kBottom = 53, kStemX = 21, kStemY = 30;
constexpr float kFace = 150;
constexpr float kCX = 32.0f, kCY = 32.0f, kDisc = 30.5f;
constexpr float kOX = 32.5f, kOY = 32.0f, kLean = 0.12f;
constexpr float kSquareOuter = 17.0f, kSquareWidth = 12.5f;
constexpr float kEOuter = 20.5f, kEWidth = 17.0f;

using Rgb = std::array<float, 3>;
using Grid = std::array<bool, N * N>;

int at(int x, int y) { return y * N + x; }
bool in(int x, int y) { return x >= 0 && y >= 0 && x < N && y < N; }
bool on_disc(int x, int y) { return std::hypot(x + 0.5f - kCX, y + 0.5f - kCY) < kDisc; }

// The texels four-connected to (x, y) through `g`.
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
    for (int i = 0; i < 256; i++)
        for (int k = 0; k < 4; k++) std::copy_n(&resource[kTexelBytes + i * 2], 2, &tmem[2048 + i * 8 + k * 2]);
    RT64::LoadTile load{};
    load.fmt = 2;  // G_IM_FMT_CI
    load.siz = 1;  // G_IM_SIZ_8b
    load.line = kW / 8;
    return RT64::TMEMHasher::hash(tmem.data(), load, kW, kH, kTlutRgba16, RT64::TMEMHasher::CurrentHashVersion);
}

} // namespace

std::vector<Texture> disc_pad(Mark::Shape shape) {
    const Mark mark = shape == Mark::Shape::Square ? Mark{shape, kSquareOuter, kSquareWidth} : Mark{shape, kEOuter, kEWidth};
    // The disc as RT64 decodes it.
    std::vector<uint8_t> resources[2];
    std::vector<Rgb> rgb(N * N);
    std::vector<uint8_t> alpha(N * N);
    for (int h = 0; h < 2; h++) {
        resources[h] = menu_art::load_resource(kHalves[h]);
        const std::vector<uint8_t>& resource = resources[h];
        if (resource.size() != size_t(kResourceBytes)) return {};
        for (int y = 0; y < kH; y++)
            for (int x = 0; x < kW; x++) {
                const uint8_t index = resource[size_t(y * kW + x) ^ ((y & 1) ? 4 : 0)];  // odd rows word-swapped
                const uint16_t v = uint16_t(resource[kTexelBytes + index * 2] << 8 | resource[kTexelBytes + index * 2 + 1]);
                for (int c = 0; c < 3; c++) {
                    const int five = (v >> (11 - 5 * c)) & 31;
                    rgb[at(h * kW + x, y)][c] = float(five << 3 | five >> 2);
                }
                alpha[at(h * kW + x, y)] = (v & 1) ? 255 : 0;
            }
    }

    // 1. The B: the bright texels in its box joined to its stem, and its counters (what of the box
    // it closes round).
    Grid bright{};
    for (int y = kTop; y <= kBottom; y++)
        for (int x = kLeft; x <= kRight; x++) {
            const Rgb& c = rgb[at(x, y)];
            bright[at(x, y)] = c[0] + c[1] + c[2] >= kFace;
        }
    const Grid letter = part_at(bright, kStemX, kStemY);
    if (!letter[at(kStemX, kStemY)]) return {};
    Grid open{};
    for (int i = 0; i < N * N; i++) open[i] = !letter[i];
    const Grid outside = part_at(open, 0, 0);
    Grid hole{};
    for (int i = 0; i < N * N; i++) hole[i] = !outside[i];
    // and a texel round it, for its blended edge.
    {
        const Grid prev = hole;
        for (int y = kTop - 1; y <= kBottom + 1; y++)
            for (int x = kLeft - 1; x <= kRight + 1; x++)
                for (int j = -1; j <= 1; j++)
                    for (int i = -1; i <= 1; i++)
                        if (in(x + i, y + j) && prev[at(x + i, y + j)]) hole[at(x, y)] = true;
    }

    // 2. Each row of the hole filled from the disc beside it in that row, mirrored outward from
    // the hole's edge: its left half from the left, its right half from the right.
    std::vector<Rgb> out = rgb;
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N;) {
            if (!hole[at(x, y)]) {
                x++;
                continue;
            }
            int end = x;
            while (end + 1 < N && hole[at(end + 1, y)]) end++;
            const int a = x, b = end;
            // The disc either side of the run in this row.
            int left_end = a - 1, right_end = b + 1;
            while (left_end - 1 >= 0 && on_disc(left_end - 1, y) && !hole[at(left_end - 1, y)]) left_end--;
            while (right_end + 1 < N && on_disc(right_end + 1, y) && !hole[at(right_end + 1, y)]) right_end++;
            const int left_width = in(a - 1, y) && on_disc(a - 1, y) ? a - left_end : 0;
            const int right_width = in(b + 1, y) && on_disc(b + 1, y) ? right_end - b : 0;
            for (int t = a; t <= b; t++) {
                const bool left = (t < a + (b - a + 1) / 2 && left_width > 0) || right_width == 0;
                if (left && left_width > 0) out[at(t, y)] = rgb[at(a - 1 - (t - a) % left_width, y)];
                else if (!left) out[at(t, y)] = rgb[at(b + 1 + (b - t) % right_width, y)];
            }
            x = end + 1;
        }

    // 3. The mark in the B's purple: the B's own texel where its face was (clear of its edges),
    // else the nearest such in the same row, blended into the disc by how much of the texel the
    // mark covers.
    std::vector<int> core;  // the B's face away from its edges and counters
    for (int y = 1; y < N - 1; y++)
        for (int x = 1; x < N - 1; x++) {
            bool all = true;
            for (int j = -1; j <= 1; j++)
                for (int i = -1; i <= 1; i++) all = all && letter[at(x + i, y + j)];
            if (all) core.push_back(at(x, y));
        }
    if (core.empty()) return {};
    Grid is_core{};
    for (int c : core) is_core[c] = true;
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            int inside = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    const float dx = x + (sx + 0.5f) / 4 - kOX, dy = y + (sy + 0.5f) / 4 - kOY;
                    inside += mark.stroke(dx + kLean * dy, dy);
                }
            if (inside == 0) continue;
            const float cover = inside / 16.0f;
            int nearest = at(x, y);
            if (!is_core[nearest]) {
                int best = 1 << 30;
                for (int c : core) {
                    const int cx = c % N - x, cy = c / N - y, d = cx * cx + 64 * cy * cy;  // rows apart count most
                    if (d < best) best = d, nearest = c;
                }
            }
            Rgb& o = out[at(x, y)];
            for (int c = 0; c < 3; c++) o[c] = o[c] * (1 - cover) + rgb[nearest][c] * cover;
        }

    std::vector<Texture> textures;
    for (int h = 0; h < 2; h++) {
        Texture t{hash(resources[h]), kW, kH, std::vector<uint8_t>(kW * kH * 4)};
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
