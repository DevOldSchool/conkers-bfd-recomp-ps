// Sharper text: the game's font, enlarged from the player's ROM, as a built-in RT64 texture pack.
#pragma once

#include <cstdint>
#include <memory>
#include <span>

namespace RT64 {
struct FileSystem;
}

namespace conker::sharp_font {

// Whether the built-in font is wanted (CONKER_SHARP_FONT, on unless "0").
bool enabled();

// An RT64 texture pack, in memory, that replaces each of the font's glyphs with a version eight
// times larger, made from the glyphs in the ROM. Null if the ROM's font is not where it should be.
std::shared_ptr<RT64::FileSystem> make_pack(std::span<const uint8_t> rom);

// With CONKER_DUMP_SHARP_FONT=<dir>, writes the pack's files to <dir> (a pack to start from).
void dump_pack(const RT64::FileSystem& pack);

} // namespace conker::sharp_font
