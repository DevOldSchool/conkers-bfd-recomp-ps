// Sharper text: the game's font, enlarged from the player's ROM, as a built-in RT64 texture pack.
#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace RT64 {
struct FileSystem;
}

namespace conker::sharp_font {

// Whether the built-in font is wanted (CONKER_SHARP_FONT, on unless "0").
bool enabled();

// An RT64 texture pack, in memory, that replaces each of the font's glyphs with a version eight
// times larger, made from the glyphs in the ROM. Null if the ROM's font is not where it should be.
std::shared_ptr<RT64::FileSystem> make_pack(std::span<const uint8_t> rom);

// One of the font's glyphs, enlarged as for the pack, with what the game lays text out by (in the
// game's texels): the glyph is `width` x `height` from its line's top plus `top`, and the next one
// starts `advance` further on.
struct LargeGlyph {
    int width = 0;
    int height = 0;
    int top = 0;
    int advance = 0;
    int scale = 0;               // pixels of `alpha` a texel
    std::vector<uint8_t> alpha;  // width x height texels, `scale` pixels each, rows top first
};

// The 95 glyphs in the game's order (D_80085930 maps characters to them); empty if the ROM's font is
// not where it should be.
std::vector<LargeGlyph> large_glyphs(std::span<const uint8_t> rom);

// With CONKER_DUMP_SHARP_FONT=<dir>, writes the pack's files to <dir> (a pack to start from).
void dump_pack(const RT64::FileSystem& pack);

} // namespace conker::sharp_font
