// The B pads on the ground (Conker's context-sensitive spots), wooden (octagonal, hexagonal and
// square), stone, teal (set in a floor), grey stone, the rug and the round stone plate, drawn from
// Rare's own textures in the ROM with `shape` where their B was: a Square for a PlayStation pad,
// an E (the key that is B) for the keyboard (src/enhancements/pad_icons/pad_mark.hpp). An Xbox
// pad's B is B, so its pads stay as they are.
#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "enhancements/pad_icons/pad_mark.hpp"

namespace conker::pad_icons {

struct Texture {
    uint64_t hash;               // RT64's, for the texture as the game loads it
    int width, height;
    std::vector<uint8_t> rgba;   // straight alpha, rows top first
};

// The wooden B pad with the mark painted where its B was (src/enhancements/pad_icons/b_pad.cpp).
std::optional<Texture> wood_pad(Mark::Shape shape);

// The stone B pad's two halves, left then right, with the mark carved where its B was
// (src/enhancements/pad_icons/stone_pad.cpp); empty if the ROM's are not where they should be.
std::vector<Texture> stone_pad(Mark::Shape shape);

// The hexagonal wooden B pad's two halves, left then right, with the mark raised where its B was
// (src/enhancements/pad_icons/hex_pad.cpp); empty if the ROM's are not where they should be.
std::vector<Texture> hex_pad(Mark::Shape shape);

// The square B pad with the mark raised where its B was (src/enhancements/pad_icons/square_pad.cpp).
std::optional<Texture> square_pad(Mark::Shape shape);

// The floor tile with a teal B pad in it, left half then right, with the mark where its B was
// (src/enhancements/pad_icons/oval_pad.cpp); empty if the ROM's are not where they should be.
std::vector<Texture> oval_pad(Mark::Shape shape);

// The grey stone B pad with the mark raised where its B was (src/enhancements/pad_icons/rock_pad.cpp).
std::optional<Texture> rock_pad(Mark::Shape shape);

// The rug B pad with the mark written where its B was (src/enhancements/pad_icons/rug_pad.cpp).
std::optional<Texture> rug_pad(Mark::Shape shape);

// The round stone B plate's four quarters (resources 308 to 311, in that order) with the mark sunk where
// its B was (src/enhancements/pad_icons/plate_pad.cpp); empty if the ROM's are not where they should be.
std::vector<Texture> plate_pad(Mark::Shape shape);

} // namespace conker::pad_icons
