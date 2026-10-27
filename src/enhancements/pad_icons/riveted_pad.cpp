// The riveted B plates (as in scene 35, The Tower) are a round iron plate, a ring of rivets round
// its edge and a raised inner ring, as one 32x64 CI8 texture with a 256-colour RGBA16 palette, flat
// resource 7227. The texture is drawn twice as wide as it is tall, so each of its rows is half a
// texel of the plate. Inside the inner ring is a fat B in near-black, filling the ring, its two
// counters small specks of brown.
//
// For a PlayStation pad or the keyboard the inside of the ring is filled with the ring's own metal
// (each row from the ring beside it), and the mark (a square, or an E for the keyboard), as fat as
// the B, is drawn on it in the B's near-black, blended into the metal at its edges. The mark is
// measured on the plate, not the texture, so it is as round in the game as the B was. It is made from the player's ROM when the game starts, so
// no part of Rare's texture is in this repository.
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

constexpr int kResource = 7227;
constexpr int kW = 32, kH = 64;
constexpr int kTexelBytes = kW * kH;
constexpr int kResourceBytes = kTexelBytes + 256 * 2;
constexpr uint32_t kTlutRgba16 = 0x8000;  // G_TT_RGBA16, as RT64 hashes a TLUT draw

// The recess inside the inner ring, in texels: its middle, its top and bottom rows, and how wide
// the ring is; the B's dark is at most kDark bright (r + g + b), the ring's metal at least kMetal. Then the mark, in the ring's middle,
// for the square and the E: its outer half-width (the E's half height) and stroke width, in
// texels across (a texel down is half that).
constexpr float kCX = 16.5f, kCY = 31.0f;
constexpr int kTop = 14, kBottom = 48, kRingWidth = 4;
constexpr float kDark = 105, kMetal = 120;
constexpr float kSquareOuter = 6.0f, kSquareWidth = 3.5f;
constexpr float kEOuter = 7.6f, kEWidth = 6.2f;

using Rgb = std::array<float, 3>;

int at(int x, int y) { return y * kW + x; }
float lum(const Rgb& c) { return c[0] + c[1] + c[2]; }

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

std::optional<Texture> riveted_pad(Mark::Shape shape) {
    const Mark mark = shape == Mark::Shape::Square ? Mark{shape, kSquareOuter, kSquareWidth} : Mark{shape, kEOuter, kEWidth};
    const std::vector<uint8_t> resource = menu_art::load_resource(kResource);
    if (resource.size() != size_t(kResourceBytes)) return std::nullopt;
    std::vector<Rgb> rgb(kW * kH);
    std::vector<uint8_t> alpha(kW * kH);
    for (int y = 0; y < kH; y++)
        for (int x = 0; x < kW; x++) {
            const uint8_t index = resource[size_t(y * kW + x) ^ ((y & 1) ? 4 : 0)];  // odd rows word-swapped
            const uint16_t v = uint16_t(resource[kTexelBytes + index * 2] << 8 | resource[kTexelBytes + index * 2 + 1]);
            for (int c = 0; c < 3; c++) {
                const int five = (v >> (11 - 5 * c)) & 31;
                rgb[at(x, y)][c] = float(five << 3 | five >> 2);
            }
            alpha[at(x, y)] = (v & 1) ? 255 : 0;
        }

    // The B's near-black, as its mean colour.
    Rgb dark{};
    int darks = 0;
    for (int y = kTop; y <= kBottom; y++)
        for (int x = 0; x < kW; x++)
            if (y >= kTop && y <= kBottom && lum(rgb[at(x, y)]) <= kDark) {
                for (int c = 0; c < 3; c++) dark[c] += rgb[at(x, y)][c];
                darks++;
            }
    if (darks == 0) return std::nullopt;
    for (float& c : dark) c /= float(darks);

    // Inside the ring, the ring's metal (each row of it from the ring beside it in that row,
    // mirrored outward, so the ring's grain carries on), and on it the mark in the B's near-black
    // (the B's own texel where it was dark, else its mean), by how much of each texel it covers,
    // sampled on the plate.
    std::vector<Rgb> out = rgb;
    for (int y = 0; y < kH; y++) {
        // The recess in this row: out from the middle to where the ring's metal starts (two texels
        // of it in a row).
        if (y < kTop || y > kBottom) continue;
        auto metal_at = [&](int x) { return x < 0 || x >= kW || lum(rgb[at(x, y)]) >= kMetal; };
        int a = int(kCX), b = int(kCX);
        while (a > 0 && !(metal_at(a - 1) && metal_at(a - 2))) a--;
        while (b < kW - 1 && !(metal_at(b + 1) && metal_at(b + 2))) b++;
        const int left = a - kRingWidth, right = b + kRingWidth;  // how far the ring reaches
        for (int x = a; x <= b; x++) {
            const bool from_left = x < (a + b + 1) / 2;
            const int k = from_left ? (x - a) % kRingWidth : (b - x) % kRingWidth;
            int source = from_left ? std::max(a - 1 - k, left) : std::min(b + 1 + k, right);
            // past the ring's dark edges to its metal
            while (source > 0 && source < kW - 1 && lum(rgb[at(source, y)]) < kMetal) source += from_left ? -1 : 1;
            if (source < 0 || source >= kW) continue;
            out[at(x, y)] = rgb[at(source, y)];
        }
    }
    // The mark, where it reaches past the recess too.
    const std::vector<Rgb> metal = out;
    for (int y = kTop; y <= kBottom; y++)
        for (int x = 0; x < kW; x++) {
            int inside = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    const float dx = x + (sx + 0.5f) / 4 - kCX, dy = (y + (sy + 0.5f) / 4 - kCY) / 2;
                    inside += mark.stroke(dx, dy);
                }
            if (inside == 0) continue;
            const float cover = inside / 16.0f;
            const int i = at(x, y);
            const Rgb& ink = lum(rgb[i]) <= kDark ? rgb[i] : dark;
            for (int c = 0; c < 3; c++) out[i][c] = metal[i][c] * (1 - cover) + ink[c] * cover;
        }

    Texture texture{hash(resource), kW, kH, std::vector<uint8_t>(kW * kH * 4)};
    for (int i = 0; i < kW * kH; i++) {
        for (int c = 0; c < 3; c++) texture.rgba[i * 4 + c] = uint8_t(std::lround(out[i][c]));
        texture.rgba[i * 4 + 3] = alpha[i];
    }
    return texture;
}

} // namespace conker::pad_icons
