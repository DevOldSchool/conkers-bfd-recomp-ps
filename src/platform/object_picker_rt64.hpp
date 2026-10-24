// What is under the mouse, for the object inspector (see object_picker_rt64.cpp).
#pragma once

namespace conker::renderer {

// Hands RT64's finished workloads to the picker. Once, when the renderer starts.
void install_object_picker();

// Before each display list task: has RT64 read the game's draw tags while the inspector is on.
void update_object_picker();

}  // namespace conker::renderer
