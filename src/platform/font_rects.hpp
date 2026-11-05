// Widens the rectangles the game draws its text font with, for replaced glyphs.
#pragma once

#include <cstdint>

namespace conker::renderer {

// Walks a graphics task's display list and extends each font glyph's TEXRECT over the empty rows
// and columns around the glyph, and starts the bubbles' one-to-one pictures on a whole texel (see
// font_rects.cpp). Rewrites the commands in RDRAM, for the renderer alone: put_back_font_rects
// gives them back their own words once the list is drawn.
void widen_font_rects(uint8_t* rdram, uint32_t display_list);

// The commands widen_font_rects rewrote, as the game wrote them. The game's memory then holds only
// what the game wrote: online every machine hashes it, and whether a machine widens (its texture
// packs, the built-in font) and where (the game's memory, or the copy the "my view" pass draws from)
// is its own.
void put_back_font_rects(uint8_t* rdram);

} // namespace conker::renderer
