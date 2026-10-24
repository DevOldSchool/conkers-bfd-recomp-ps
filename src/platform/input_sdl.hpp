// SDL side of the controller ports (src/platform/input_sdl.cpp). All three run on the main thread, from the
// window's event loop in src/platform/window_sdl.cpp.
#pragma once

#include <SDL.h>

namespace conker::input {

// Starts SDL's game-controller subsystem and gives each connected controller a port, up to four.
void init();
// Hot-plug: a controller that connects takes the lowest free port; one that is removed frees its
// port, and the others keep theirs.
void handle_event(const SDL_Event& event);
// Samples the open controllers and the keyboard into the state read() returns.
void update();

} // namespace conker::input
