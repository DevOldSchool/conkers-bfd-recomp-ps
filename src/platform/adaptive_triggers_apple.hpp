// DualSense adaptive triggers through Apple's GameController framework
// (src/platform/adaptive_triggers_apple.mm), for the pads SDL reads through it on macOS.
#pragma once

#include "platform/input.hpp"

namespace conker::input::apple {

// Sets a port's DualSense's triggers (0 is controller 1): the effect on the left and/or right one
// (those that are Z), the other let free; false when it has none (or macOS is older than 12.3).
// Main thread.
bool set_dualsense_triggers(int port, const TriggerEffect& effect, bool left, bool right);

}  // namespace conker::input::apple
