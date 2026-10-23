// What the game culls to its 4:3 view, culled to the widened view instead
// (src/enhancements/display_options/widescreen_culling.cpp).
#pragma once

namespace conker::display {

// How much wider than the game's 4:3 view the culling reaches: the rendered picture's shape over
// the picture's 292:216, with a little to spare, or 1 in 4:3 and while WIDESCREEN CULLING is off.
float culling_widening();

}  // namespace conker::display
