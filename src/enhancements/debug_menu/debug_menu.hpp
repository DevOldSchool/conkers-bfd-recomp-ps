// The debug menu's CONTROLS page, opened on its own from the OPTIONS page's CONTROLS row
// (src/enhancements/display_options): it takes the OPTIONS page's place, the world going on behind
// it (no pause), and B goes back to OPTIONS. Not online, where only this machine's game would stop
// reading the pad and drawing OPTIONS, setting it apart from the others.
#pragma once

namespace conker::debug_menu {

bool controls_available();

// Opens the page at the next frame, on a menu screen; game thread.
void open_controls();

}  // namespace conker::debug_menu
