// Debug views the renderer can switch on, for the debug menu (src/enhancements/debug_menu).
#pragma once

namespace conker::renderer {

// Draws the game's 3D triangles as their edges; 2D rectangles (text, sprites) stay solid. Metal
// only (tools/patches/rt64-wireframe.patch, plume-wireframe.patch); the null build ignores it.
// Safe to call from any thread.
void set_wireframe(bool on);

} // namespace conker::renderer
