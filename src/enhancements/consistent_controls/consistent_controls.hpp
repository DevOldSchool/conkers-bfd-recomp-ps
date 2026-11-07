// CONSISTENT CONTROLS (src/enhancements/consistent_controls/consistent_controls.cpp).
#pragma once

namespace conker::consistent_controls {

// This machine's CONSISTENT CONTROLS switch. Online each player's goes with their port
// (frame_input::Port::kConsistentControls), and the remap follows the port's.
bool enabled();

// Whether this machine's pad aimed in the last pad hook (a sight, a gun, R's look, a mounted gun): the
// right stick is scaled by STICK AIM only then, not while it turns the view.
bool stick_aiming(int pad);

}  // namespace conker::consistent_controls
