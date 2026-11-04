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
bool take_aim(int port, float* x, float* y);

// Through a sight that zooms with C-up and C-down (a match's knives): +1 to hold C-up (in), -1 C-down
// (out), 0 neither, this frame. Each wheel step (or R/T) holds one for a few frames.
int take_sight_zoom(int port);

// Whether the port's mouse aimed (take_aim) in the last pad hook: the CONTROLS page's MOUSE AIM speed
// then scales its motion as it goes into the next frame's record (src/runtime/frame_input.cpp).
bool aiming(int port);

// Whether the game is being played this frame (no menu, pause or inspector), as frame() saw it.
bool playing();

// This frame's sideways motion as degrees to turn right, which the camera then does not take.
// 0 when the mouse is not captured.
float take_turn(int port);

// Turns the camera of the player on this port this many degrees to the right this frame, even
// where the game locks its swing (a multiplayer match; consistent_controls.cpp turns the player
// with it).
void turn_camera(int port, float degrees);

}  // namespace conker::mouse_look
