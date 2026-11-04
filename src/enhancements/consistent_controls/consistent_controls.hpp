// CONSISTENT CONTROLS (src/enhancements/consistent_controls/consistent_controls.cpp).
#pragma once

namespace conker::consistent_controls {

// This machine's CONSISTENT CONTROLS switch. Online each player's goes with their port
// (frame_input::Port::kConsistentControls), and the remap follows the port's.
bool enabled();

}  // namespace conker::consistent_controls
