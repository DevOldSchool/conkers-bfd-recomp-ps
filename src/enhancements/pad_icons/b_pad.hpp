// The B pads on the ground (Conker's context-sensitive spots), wooden (octagonal and hexagonal) and
// stone, drawn for a PlayStation pad from Rare's own textures in the ROM. An Xbox pad's B is B, so
// its pads stay as they are.
#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace conker::pad_icons {

struct Texture {
    uint64_t hash;               // RT64's, for the texture as the game loads it
    int width, height;
    std::vector<uint8_t> rgba;   // straight alpha, rows top first
};

// The wooden B pad with a circle painted where its B was (src/enhancements/pad_icons/b_pad.cpp).
std::optional<Texture> circle_pad();

// The stone B pad's two halves, left then right, with an O carved where its B was
// (src/enhancements/pad_icons/stone_pad.cpp); empty if the ROM's are not where they should be.
std::vector<Texture> circle_stone_pad();

// The hexagonal wooden B pad's two halves, left then right, with a raised O where its B was
// (src/enhancements/pad_icons/hex_pad.cpp); empty if the ROM's are not where they should be.
std::vector<Texture> circle_hex_pad();

} // namespace conker::pad_icons
