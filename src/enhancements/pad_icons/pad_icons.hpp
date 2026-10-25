// Button pictures for the pad in use: the game's N64 button pictures drawn as an Xbox or PlayStation
// pad's or the keyboard's keys, or redrawn sharper as the N64's own, as a built-in RT64 texture pack.
#pragma once

#include <memory>

namespace RT64 {
struct FileSystem;
}

namespace conker::pad_icons {

// Original: Rare's own pictures, left as they are. N64: the same buttons redrawn. Keyboard: the
// keys that press them (src/platform/input_sdl.cpp).
enum class Style { Original, N64, Xbox, PlayStation, Keyboard };
constexpr int kStyles = 5;

// The pictures for controller 1 now: by the kind of pad connected (the keyboard's keys while no pad
// is, the redrawn N64 buttons for pads that are neither Xbox nor PlayStation ones), or as
// CONKER_PAD_ICONS says: xbox, playstation, keyboard, n64, or 0 for Rare's; Rare's too while the
// debug menu's PAD BUTTONS is off. Safe to call from any thread
// (src/enhancements/pad_icons/choice.cpp).
Style style();

// "original", "N64", "Xbox", "PlayStation" or "keyboard".
const char* name(Style style);

// An RT64 texture pack, in memory, that replaces the button tiles with `style`'s pictures, keyed
// by the tiles' hashes in the ROM, its C buttons pointing the way to push the right stick for
// `camera_swapped` (input::camera_swapped). Null for Rare's own or if the ROM's tiles are not
// where they should be.
std::shared_ptr<RT64::FileSystem> make_pack(Style style, bool camera_swapped);

} // namespace conker::pad_icons
