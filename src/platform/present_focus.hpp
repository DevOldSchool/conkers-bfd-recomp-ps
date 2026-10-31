// What the renderer presents for the online "my view" (src/enhancements/my_view).
#pragma once

#include <cstdint>
#include <vector>

namespace conker::renderer {

// Fractions of the frame buffer (x, y, width, height); a width or height of 0 presents the whole
// picture. Safe to call from any thread; the next presented frame uses it. The null build ignores it.
void set_present_focus(float x, float y, float width, float height);

// The game memory the next frame's display list is drawn from in place of RDRAM's own: the online
// "my view" draws the local player's view again into a copy of the game's memory, which the game's
// memory is then put back from. `image` covers RDRAM from its start (4 MiB or more); the renderer
// keeps it for the next frame it is sent. The null build ignores it.
void draw_next_frame_from(std::vector<uint8_t> image);

}  // namespace conker::renderer
