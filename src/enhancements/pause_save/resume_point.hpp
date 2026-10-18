#pragma once
// Exact-position saves: see src/enhancements/pause_save/resume_point.cpp.
#include <cstdint>

namespace conker::resume {

// Remember where Conker is now; it is stored with the next save the game writes for `slot`.
void remember_position(uint8_t* rdram, int slot);

// Reload `slot` from the save and start it where it was saved, as PLAY on the title menu does.
void request_load(uint8_t* rdram, int slot);

}  // namespace conker::resume
