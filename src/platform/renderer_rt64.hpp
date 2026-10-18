// RT64-backed renderer context (built when CONKER_RENDERER=rt64).
#pragma once

#include <memory>

#include "ultramodern/renderer_context.hpp"

namespace conker::renderer {

std::unique_ptr<ultramodern::renderer::RendererContext> create_rt64_render_context(
    uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode);

} // namespace conker::renderer
