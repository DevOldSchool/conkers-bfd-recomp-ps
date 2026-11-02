// Every multiplayer character unlocked (src/enhancements/mp_characters/mp_characters.cpp).
#pragma once

#include <cstdint>

namespace conker::mp_characters {

// Once a game frame, before the logic: enters the multiplayer character cheats, or takes back the
// ones it entered once switched off.
void frame(uint8_t* rdram);

}  // namespace conker::mp_characters
