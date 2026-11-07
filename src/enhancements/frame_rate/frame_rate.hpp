// The 60 fps mode and smooth motion (frame_rate.cpp), for other enhancements that count frames and
// for the OPTIONS page's FRAME RATE row.
#pragma once

#include <cstdint>

namespace conker::frame_rate {

// Whether this frame takes a 30 Hz step: always at 30 fps, and on every second frame at 60 fps.
// Code that waits a number of frames counts only these, so it waits as long at either rate.
bool thirty_hz_step(uint8_t* rdram);

// Whether the game runs at 60 fps (off unless conker-recomp-data/frame_rate.cfg or CONKER_60FPS=1
// says otherwise). An online session plays at the host's.
bool sixty_fps();

// Smooth motion: RT64 draws pictures between the game's frames, up to the display's refresh rate
// (src/platform/renderer_rt64.cpp). Only what this machine shows; the game itself is unchanged.
bool smooth();

// The FRAME RATE row's settings. ThirtySmooth is only reached through CONKER_* variables or the
// debug menu; the row offers the other three.
enum class Choice { Thirty, Sixty, SixtySmooth, ThirtySmooth };
Choice choice();
// Sets and saves them (conker-recomp-data/frame_rate.cfg).
void request_choice(Choice choice);

}  // namespace conker::frame_rate
