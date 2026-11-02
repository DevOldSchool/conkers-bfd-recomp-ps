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

// Held by the game thread while it copies the game's memory, draws in it and puts it back (the
// online "my view"): the renderer writes nothing into RDRAM meanwhile, as a frame it finishes then
// would have its frame buffers put back to what they were before, and RT64 took those as drawn by
// the CPU and showed the game's 4:3 picture. Without the fixed step the renderer can be drawing the
// last frame while the game makes the next. The null build has nothing to wait for.
void hold_game_memory();
void release_game_memory();

}  // namespace conker::renderer
