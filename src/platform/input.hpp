// Player input for controller 1. The RT64 build reads an SDL game controller and the keyboard
// (src/platform/input_sdl.cpp, pumped by the window's event loop); the null build has no input
// device (src/platform/input_headless.cpp).
#pragma once

#include <cstdint>
#include <string>

namespace conker::input {

// N64 button bits (OSContPad.button).
constexpr uint16_t kA = 0x8000;
constexpr uint16_t kB = 0x4000;
constexpr uint16_t kZ = 0x2000;
constexpr uint16_t kStart = 0x1000;
constexpr uint16_t kDUp = 0x0800;
constexpr uint16_t kDDown = 0x0400;
constexpr uint16_t kDLeft = 0x0200;
constexpr uint16_t kDRight = 0x0100;
constexpr uint16_t kL = 0x0020;
constexpr uint16_t kR = 0x0010;
constexpr uint16_t kCUp = 0x0008;
constexpr uint16_t kCDown = 0x0004;
constexpr uint16_t kCLeft = 0x0002;
constexpr uint16_t kCRight = 0x0001;

// Current state of controller 1, safe to call from any thread. The stick is in [-1, 1] with
// +y up; ultramodern scales it to the N64 range (about +-82 on the axes).
void read(uint16_t* buttons, float* x, float* y);

// Controller 1's name, or "" when none is connected (keyboard only). Safe to call from any thread.
std::string controller_name();

// True once for each press of the debug menu button: the pad's Back (Create/Share on a
// PlayStation pad, View on an Xbox pad) or the ` key. Safe to call from any thread.
bool take_debug_menu_press();

// Controller 1's rumble pak: present while a pad that can rumble is connected. set_rumble turns
// the pak's motor on or off; both are safe to call from any thread.
bool has_rumble();
void set_rumble(bool on);

} // namespace conker::input
