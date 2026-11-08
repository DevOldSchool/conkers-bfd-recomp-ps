// Haptic effects on a DualSense through Apple's GameController framework and Core Haptics
// (src/platform/haptics_apple.mm), for the pads SDL reads through it on macOS.
#pragma once

#include "platform/input.hpp"

#ifdef __OBJC__
@class GCController;
#endif

namespace conker::input::apple {

// Plays the effect on a port's DualSense (0 is controller 1); false when it has none. Main thread.
bool play_dualsense_haptic(int port, const Haptic& haptic);

// Starts or stops the rumble pak's rumble on both grips of a port's DualSense: a deep, steady
// rumble rather than SDL's buzz. false when it has no DualSense to play it. Main thread.
bool set_dualsense_rumble(int port, bool on, const RumbleFeel& feel = {});

#ifdef __OBJC__
// A port's pad among the GameController framework's DualSense pads, or nil: the one named as SDL
// names the port's pad (SDL takes the framework's vendorName), the one SDL numbered with the port
// when several share that name (input_sdl.cpp sets each pad's player index to its port), or for
// controller 1 the only DualSense. Main thread.
GCController* port_dualsense(int port);
#endif

}  // namespace conker::input::apple
