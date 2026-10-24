// DualSense adaptive triggers through Apple's GameController framework
// (src/platform/adaptive_triggers_apple.mm), for the pads SDL reads through it on macOS.
#pragma once

#include "platform/input.hpp"

namespace conker::input::apple {

// Sets both triggers of controller 1's DualSense; false when it has none (or macOS is older than
// 12.3). Main thread.
bool set_dualsense_triggers(const TriggerEffect& effect);

}  // namespace conker::input::apple
