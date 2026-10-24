// Conker's black and white frames, greyed by RT64 instead of the CPU (src/fixes/grey_frames.cpp).
#pragma once

#include <cstdint>

namespace conker::grey_frames {

// RT64 greys the frames: the game's CPU pass is skipped.
void use_gpu(bool on);

// For the graphics task with display list `data_ptr`: the RDRAM address of its frame's colour
// image if the game will grey that frame at its swap, else 0.
uint32_t frame_to_grey(const uint8_t* rdram, uint32_t data_ptr);

}  // namespace conker::grey_frames
