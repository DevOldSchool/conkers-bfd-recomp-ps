// More bots in a multiplayer match with three or four players (src/enhancements/more_bots/more_bots.cpp).
#pragma once

#include <cstdint>

namespace conker::more_bots {

// Once a game frame, before the logic: the character limits raised, or put back once switched off.
void frame(uint8_t* rdram);

}  // namespace conker::more_bots
