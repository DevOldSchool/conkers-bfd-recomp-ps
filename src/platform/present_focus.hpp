// A part of the frame buffer the renderer presents on its own, scaled to fit the window
// (tools/patches/rt64-present-focus.patch): the online "my view" (src/enhancements/my_view).
#pragma once

namespace conker::renderer {

// Fractions of the frame buffer (x, y, width, height); a width or height of 0 presents the whole
// picture. Safe to call from any thread; the next presented frame uses it. The null build ignores it.
void set_present_focus(float x, float y, float width, float height);

}  // namespace conker::renderer
