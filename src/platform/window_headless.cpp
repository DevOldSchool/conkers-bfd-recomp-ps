// No window: the null renderer draws nothing, so the runtime stays headless.
#include "platform/window.hpp"

namespace conker::window {

void* create_gfx() { return nullptr; }
ultramodern::renderer::WindowHandle create_window(void* /*gfx_data*/) { return {}; }
void update_gfx(void* /*gfx_data*/) {}
void request_quit() { ultramodern::quit(); }

} // namespace conker::window
