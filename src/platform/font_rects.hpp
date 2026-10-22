// Widens the rectangles the game draws its text font with, for replaced glyphs.
#pragma once

#include <cstdint>

namespace conker::renderer {

// Walks a graphics task's display list and extends each font glyph's TEXRECT over the empty rows
// and columns around the glyph, and starts the bubbles' one-to-one pictures on a whole texel (see
// font_rects.cpp). Rewrites the commands in RDRAM.
void widen_font_rects(uint8_t* rdram, uint32_t display_list);

} // namespace conker::renderer
