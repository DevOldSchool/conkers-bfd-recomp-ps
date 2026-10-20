// The pause menu's SAVE and LOAD signs (src/enhancements/pause_save), lettered with Rare's own
// letters from the game's other green signs.
#pragma once

#include <array>

#include "enhancements/sharp_menus/menu_art.hpp"

namespace conker::pause_signs {

enum Sign { kSave = 0, kLoad = 1 };
enum Variant { kUnlit = 0, kLit = 1 };

struct Signs {
    // 4x: 256x128, for the sharper menus' texture pack.
    menu_art::Image large[2][2];  // [sign][variant]
    // At the game's size: the left and right 32x32 tiles, as stored for the game to load.
    std::array<menu_art::StoredTile, 2> tiles[2][2];
};

// Made from the ROM on first use; null if its signs are not what they should be.
const Signs* get();

} // namespace conker::pause_signs
