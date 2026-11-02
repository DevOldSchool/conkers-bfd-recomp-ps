// The red wooden B tiles (as in scene 10, Bats Tower) are a dark board sunk in a bevelled frame,
// split into two 32x64 CI8 textures with their own 256-colour RGBA16 palettes, flat resources 1083
// (left) and 1084 (right). A fat B of paler wood lies on the board turned 56 degrees clockwise (its
// stem along the top left, its bowls toward the lower right), its counters small dark holes. Board
// and B are both lit brighter toward the top right.
//
// For a PlayStation pad or the keyboard the B and its counters are taken out and the board under
// them filled: its smooth colour (a blur) carried smoothly across from round the B, and on that the
// board's grain, copied in 3x3 blocks from the board round it. Then the mark (a square, or an E for
// the keyboard), as fat as the B and turned as it is, is laid where the B was in the B's own wood:
// the same texel where the B's face was, else the nearest of it, so its light and grain carry on,
// blended into the board at its edges. It is made from the player's ROM when the game starts, so
// no part of Rare's texture is in this repository.
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

constexpr int kHalves[2] = {1083, 1084};  // left, right
constexpr int kW = 32, kH = 64;           // each half
constexpr int N = 64;                     // the whole tile
constexpr int kTexelBytes = kW * kH;
constexpr int kResourceBytes = kTexelBytes + 256 * 2;
constexpr uint32_t kTlutRgba16 = 0x8000;  // G_TT_RGBA16, as RT64 hashes a TLUT draw

// The board inside the frame's bevels (inclusive), a texel of the B's stem and how bright (r + g + b)
// its face is at the least. Then the mark, where the B is and as heavy as it: its middle, how far it
// is turned (clockwise, in degrees) and for the square and the E its outer half-width (the E's half
// height) and stroke width, in texels. The mark is centred on the board, a little above where the B's
// middle was.
struct Box {
    int left, top, right, bottom;
};
constexpr Box kBoard = {7, 6, 57, 56};
constexpr int kStemX = 30, kStemY = 25;
constexpr float kFace = 160;
constexpr int kBlur = 2;        // the smooth colour's reach either way, in texels
constexpr int kGrainBlock = 3;  // the grain is copied in blocks this many texels square
constexpr float kOX = 33.0f, kOY = 31.0f, kTurn = 56.0f;
constexpr float kSquareOuter = 17.0f, kSquareWidth = 11.5f;
constexpr float kEOuter = 21.0f, kEWidth = 15.0f;

using Rgb = std::array<float, 3>;
using Grid = std::array<bool, N * N>;

int at(int x, int y) { return y * N + x; }
bool in(int x, int y) { return x >= 0 && y >= 0 && x < N && y < N; }
bool in_box(const Box& b, int x, int y) { return x >= b.left && x <= b.right && y >= b.top && y <= b.bottom; }

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

std::vector<Texture> tile_pad(Mark::Shape shape) {
    const Mark mark = shape == Mark::Shape::Square ? Mark{shape, kSquareOuter, kSquareWidth} : Mark{shape, kEOuter, kEWidth};
    // The tile as RT64 decodes it.
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

    // 1. The B: the bright texels on the board joined to its stem, and its counters (what of the
    // board it closes round), and a texel round them, for the mark's blended edge.
    Grid bright{};
    for (int y = kBoard.top; y <= kBoard.bottom; y++)
        for (int x = kBoard.left; x <= kBoard.right; x++) {
            const Rgb& c = rgb[at(x, y)];
            bright[at(x, y)] = c[0] + c[1] + c[2] >= kFace;
        }
    const Grid letter = part_at(bright, kStemX, kStemY);
    if (!letter[at(kStemX, kStemY)]) return {};
    Grid open{};
    for (int i = 0; i < N * N; i++) open[i] = !letter[i];
    const Grid outside = part_at(open, kBoard.left, kBoard.top);
    Grid hole{};
    for (int y = kBoard.top; y <= kBoard.bottom; y++)
        for (int x = kBoard.left; x <= kBoard.right; x++)
            for (int j = -1; j <= 1; j++)
                for (int i = -1; i <= 1; i++)
                    if (in(x + i, y + j) && !outside[at(x + i, y + j)]) hole[at(x, y)] = true;

    // 2. The board's smooth colour (a blur of the board round the B), carried smoothly across the
    // hole, and its grain (how far each texel is off the smooth colour) laid on that in 3x3 blocks,
    // each copied from a block picked at random among those of the board clear of the B.
    std::vector<Rgb> smooth(N * N);
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            Rgb sum{};
            int count = 0;
            for (int j = -kBlur; j <= kBlur; j++)
                for (int i = -kBlur; i <= kBlur; i++) {
                    if (!in_box(kBoard, x + i, y + j) || hole[at(x + i, y + j)]) continue;
                    for (int c = 0; c < 3; c++) sum[c] += rgb[at(x + i, y + j)][c];
                    count++;
                }
            for (int c = 0; c < 3; c++) smooth[at(x, y)][c] = count ? sum[c] / float(count) : rgb[at(x, y)][c];
        }
    std::vector<Rgb> board = smooth;
    for (int pass = 0; pass < 2000; pass++)
        for (int y = kBoard.top; y <= kBoard.bottom; y++)
            for (int x = kBoard.left; x <= kBoard.right; x++) {
                if (!hole[at(x, y)]) continue;
                Rgb& b = board[at(x, y)];
                for (int c = 0; c < 3; c++)
                    b[c] = (board[at(x - 1, y)][c] + board[at(x + 1, y)][c] + board[at(x, y - 1)][c] + board[at(x, y + 1)][c]) / 4;
            }
    std::vector<int> blocks;  // their top left texels
    for (int y = kBoard.top; y + kGrainBlock - 1 <= kBoard.bottom; y++)
        for (int x = kBoard.left; x + kGrainBlock - 1 <= kBoard.right; x++) {
            bool all = true;
            for (int j = 0; j < kGrainBlock; j++)
                for (int i = 0; i < kGrainBlock; i++) all = all && !hole[at(x + i, y + j)];
            if (all) blocks.push_back(at(x, y));
        }
    if (blocks.empty()) return {};
    std::mt19937 random(3);
    std::vector<Rgb> out = rgb;
    for (int y = kBoard.top; y <= kBoard.bottom; y += kGrainBlock)
        for (int x = kBoard.left; x <= kBoard.right; x += kGrainBlock) {
            const int from = blocks[random() % blocks.size()];
            for (int j = 0; j < kGrainBlock; j++)
                for (int i = 0; i < kGrainBlock; i++) {
                    if (!in_box(kBoard, x + i, y + j) || !hole[at(x + i, y + j)]) continue;
                    const int to = at(x + i, y + j), source = from + j * N + i;
                    for (int c = 0; c < 3; c++) out[to][c] = board[to][c] + rgb[source][c] - smooth[source][c];
                }
        }

    // 3. The mark in the B's wood: the B's own texel where its face was (clear of its edges), else
    // the nearest such, blended into the board by how much of the texel the mark covers.
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
    const float turn = kTurn * 3.14159265f / 180.0f, cs = std::cos(turn), sn = std::sin(turn);
    for (int y = kBoard.top; y <= kBoard.bottom; y++)
        for (int x = kBoard.left; x <= kBoard.right; x++) {
            int inside = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    const float dx = x + (sx + 0.5f) / 4 - kOX, dy = y + (sy + 0.5f) / 4 - kOY;
                    inside += mark.stroke(dx * cs + dy * sn, dy * cs - dx * sn);  // in the B's own turn
                }
            if (inside == 0) continue;
            const float cover = inside / 16.0f;
            int nearest = at(x, y);
            if (!is_core[nearest]) {
                int best = 1 << 30;
                for (int c : core) {
                    const int cx = c % N - x, cy = c / N - y, d = cx * cx + cy * cy;
                    if (d < best) best = d, nearest = c;
                }
            }
            Rgb& o = out[at(x, y)];
            for (int c = 0; c < 3; c++) o[c] = o[c] * (1 - cover) + rgb[nearest][c] * cover;
        }
    for (Rgb& o : out)
        for (float& c : o) c = std::clamp(c, 0.0f, 255.0f);

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
