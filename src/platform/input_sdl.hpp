// SDL side of controller 1 (src/platform/input_sdl.cpp). All three run on the main thread, from the
// window's event loop in src/platform/window_sdl.cpp.
#pragma once

#include <SDL.h>

namespace conker::input {

// Starts SDL's game-controller subsystem and opens the first controller, if any.
void init();
// Hot-plug: opens a controller when one is connected and none is open, closes it on removal.
void handle_event(const SDL_Event& event);
// Samples the open controller and the keyboard into the state read() returns.
void update();

} // namespace conker::input
