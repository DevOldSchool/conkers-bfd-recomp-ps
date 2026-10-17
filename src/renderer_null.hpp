// A renderer that accepts display lists and draws nothing. It lets the recompiled game run
// headless while RT64 gains support for Conker's F3DEX2CBFD microcode (docs/rsp-microcode.md).
#pragma once

#include <memory>

#include "ultramodern/renderer_context.hpp"

namespace conker::renderer {

std::unique_ptr<ultramodern::renderer::RendererContext> create_null_render_context(
    uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode);

} // namespace conker::renderer
