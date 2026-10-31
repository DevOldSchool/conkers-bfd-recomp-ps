// The experimental 60 fps mode (frame_rate.cpp), for other enhancements that count frames.
#pragma once

#include <cstdint>

namespace conker::frame_rate {

// Whether this frame takes a 30 Hz step: always at 30 fps, and on every second frame at 60 fps
// (CONKER_60FPS=1). Code that waits a number of frames counts only these, so it waits as long at
// either rate.
bool thirty_hz_step(uint8_t* rdram);

// Whether the 60 fps mode is on (CONKER_60FPS=1, or switched on in the debug menu).
bool sixty_fps();

}  // namespace conker::frame_rate
