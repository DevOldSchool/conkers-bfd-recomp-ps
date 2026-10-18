// A renderer that accepts display lists and draws nothing (the default, CONKER_RENDERER=null).
// It lets the recompiled game run headless; CONKER_RENDERER=rt64 draws them (renderer_rt64.hpp).
#pragma once

#include <memory>

#include "ultramodern/renderer_context.hpp"

namespace conker::renderer {

std::unique_ptr<ultramodern::renderer::RendererContext> create_null_render_context(
    uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode);

} // namespace conker::renderer
