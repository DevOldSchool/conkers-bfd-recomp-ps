// Online "my view": in a match each machine shows only its own
// player's view, scaled up to the window, and plays the sounds as that player hears them.
#pragma once

#include <cstdint>

#include "recomp.h"

namespace conker::my_view {

// Once a game frame on the game thread, at the start of func_1501905C before the frame's audio frame
// is made: picks what the renderer presents and brings the playing sounds to the local listener.
void frame(uint8_t* rdram, recomp_context* ctx);

// While the local view is drawn again (into memory that is put back after): native hooks that
// keep their own state across frames leave it alone.
bool drawing_again();

}  // namespace conker::my_view
