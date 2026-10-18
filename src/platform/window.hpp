// Window and event-loop callbacks for ultramodern's gfx_callbacks_t. The null renderer uses the
// headless versions; the RT64 build opens an SDL window (src/platform/window_sdl.cpp).
#pragma once

#include "ultramodern/ultramodern.hpp"

namespace conker::window {

void* create_gfx();
ultramodern::renderer::WindowHandle create_window(void* gfx_data);
void update_gfx(void* gfx_data);

} // namespace conker::window
