// Debug views the renderer can switch on, for the debug menu (src/enhancements/debug_menu).
#pragma once

#include <string>
#include <vector>

namespace conker::renderer {

// Draws the game's 3D triangles as their edges; 2D rectangles (text, sprites) stay solid. Metal
// only (tools/patches/rt64-wireframe.patch, plume-wireframe.patch); the null build ignores it.
// Safe to call from any thread.
void set_wireframe(bool on);

// What was loaded over the game's textures: the built-in sharp font and the packs in
// conker-recomp-data/texture_packs, by name (none in the null build, or before the first frame).
std::vector<std::string> texture_packs();

// The GPU drawing the game and the graphics API used ("" in the null build, or before setup).
std::string gpu_name();
std::string graphics_api();

} // namespace conker::renderer
