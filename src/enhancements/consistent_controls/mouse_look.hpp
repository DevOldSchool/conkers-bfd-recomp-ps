// The mouse in the game (src/enhancements/consistent_controls/mouse_look.cpp): it turns the
// third-person camera, aims where the right stick aims, and the wheel (or R and T) zooms.
#pragma once

#include <cstdint>

namespace conker::mouse_look {

// Once a frame, from the pad hook (func_1501BBB8), before the pad is rewritten: tells the input
// side whether the game is being played, and takes the mouse's motion and zoom for this frame.
void frame(uint8_t* rdram);

// This frame's motion as a stick deflection (+x right, +y up), for aiming, which then takes it
// from the camera. False when the mouse is not captured.
bool take_aim(float* x, float* y);

}  // namespace conker::mouse_look
