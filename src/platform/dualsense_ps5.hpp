// A DualSense's adaptive triggers and haptics on the PS5 (src/platform/dualsense_ps5.cpp), which the
// console's SDL does not reach: it opens each pad in the compatible two-motor mode and refuses trigger
// effects. As src/platform/haptics_apple.mm and adaptive_triggers_apple.mm do on macOS.
#pragma once

#include <string>

#include "platform/input.hpp"

namespace conker::input::ps5 {

// A port's pad came (SDL names a PS5 pad after its user): finds its pad handle and opens the pad's
// vibration audio port. False when the pad cannot play haptics this way (the motors then do).
// Main thread, as the others.
bool open_dualsense(int port, const std::string& sdl_name);
void close_dualsense(int port);

// Whether the pad of the user SDL named a controller after is switched on. The console's SDL lists every
// signed-in user as a controller, pad or not; one without a pad must not hold a port (players follow
// the order pads are switched on). True when it cannot tell. Main thread.
bool pad_connected(const std::string& sdl_name);

// Sets both triggers of the port's pad; false when it has none.
bool set_dualsense_triggers(int port, const TriggerEffect& effect);

// Plays a shaped effect on the pad's actuators; false when they cannot (the motors then do).
bool play_dualsense_haptic(int port, const Haptic& haptic);

// Starts or stops the rumble pak's rumble, a deep rumble in both grips; false when it cannot.
bool set_dualsense_rumble(int port, bool on, const RumbleFeel& feel = {});

}  // namespace conker::input::ps5
