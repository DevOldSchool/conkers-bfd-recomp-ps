// Haptic effects on a DualSense through Apple's GameController framework and Core Haptics
// (src/platform/haptics_apple.mm), for the pads SDL reads through it on macOS.
#pragma once

#include "platform/input.hpp"

#ifdef __OBJC__
@class GCController;
#endif

namespace conker::input::apple {

// Plays the effect on controller 1's DualSense; false when it has none. Main thread.
bool play_dualsense_haptic(const Haptic& haptic);

// Starts or stops the rumble pak's rumble on both grips of controller 1's DualSense: a deep, steady
// rumble rather than SDL's buzz. false when it has no DualSense to play it. Main thread.
bool set_dualsense_rumble(bool on);

#ifdef __OBJC__
// Controller 1's pad among the GameController framework's DualSense pads, or nil: the one named as
// SDL names controller 1 (SDL takes the framework's vendorName), the one SDL numbered player 1 when
// several share that name (input_sdl.cpp sets each pad's player index to its port), or the only
// DualSense. Main thread.
GCController* port1_dualsense();
#endif

}  // namespace conker::input::apple
