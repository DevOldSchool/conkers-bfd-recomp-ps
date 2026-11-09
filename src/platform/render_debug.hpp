// Debug views the renderer can switch on, for the debug menu (src/enhancements/debug_menu).
#pragma once

#include <string>
#include <vector>

namespace conker::renderer {

// What is loaded over the game's textures: the built-in sharp font and menus (while the OPTIONS
// menu's SHARP TEXT is chosen) and the packs in conker-recomp-data/texture_packs, by name (none in
// the null build, or before the first frame).
std::vector<std::string> texture_packs();

// The GPU drawing the game and the graphics API used ("" in the null build, or before setup).
std::string gpu_name();
std::string graphics_api();

} // namespace conker::renderer
