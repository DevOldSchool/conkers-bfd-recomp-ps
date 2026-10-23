// Haptic effects on a DualSense through Apple's GameController framework and Core Haptics
// (src/platform/haptics_apple.mm), for the pads SDL reads through it on macOS.
#pragma once

#include "platform/input.hpp"

namespace conker::input::apple {

// Plays the effect on the connected DualSense; false when there is none. Main thread.
bool play_dualsense_haptic(const Haptic& haptic);

}  // namespace conker::input::apple
