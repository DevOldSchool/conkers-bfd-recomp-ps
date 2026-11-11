// conker-recomp-data/input.cfg: the controls' settings, a "key=value" line each. The OPTIONS page's
// MOUSE AIM (src/runtime/frame_input.cpp) and the debug menu's CONTROLS page (the button mapping in
// src/platform/input_sdl.cpp, the invert settings in frame_input.cpp) keep theirs here, so each
// write keeps the others' lines. Safe to call from any thread.
#pragma once

#include <string>

namespace conker::input_config {

// The value kept for a key, or "" when the file has none.
std::string get(const std::string& key);

// Keeps a value for a key (an empty one takes the key out) and writes the file.
void set(const std::string& key, const std::string& value);

// Reads the file again, where the data folder is now. Other files' globals ask for their values as
// they are made, before main; a launch that then enters its data folder (src/platform/launch.hpp) had
// read the file of the folder it was started in (none, from the desktop), so every control setting
// was the default each run, and the first one changed wrote the real file over with only itself.
void reload();

}  // namespace conker::input_config
