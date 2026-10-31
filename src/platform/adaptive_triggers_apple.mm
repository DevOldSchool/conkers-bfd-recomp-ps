// DualSense adaptive triggers through Apple's GameController framework. On macOS SDL reads
// PlayStation pads through that framework (src/platform/input_sdl.cpp), and SDL can't send
// trigger effects that way, so input_sdl.cpp sets them here instead.
#include "platform/adaptive_triggers_apple.hpp"
#include "platform/haptics_apple.hpp"

#import <GameController/GameController.h>

namespace conker::input::apple {

namespace {

void apply(GCDualSenseAdaptiveTrigger* trigger, const TriggerEffect& effect) API_AVAILABLE(macos(12.3)) {
    switch (effect.mode) {
        case TriggerEffect::Mode::Feedback: {
            GCDualSenseAdaptiveTriggerPositionalResistiveStrengths strengths{};
            for (int i = 0; i < kTriggerZones; i++) {
                strengths.values[i] = effect.zones[i];
            }
            [trigger setModeFeedbackWithResistiveStrengths:strengths];
            break;
        }
        case TriggerEffect::Mode::Weapon:
            [trigger setModeWeaponWithStartPosition:effect.start endPosition:effect.end resistiveStrength:effect.strength];
            break;
        case TriggerEffect::Mode::Vibration:
            [trigger setModeVibrationWithStartPosition:effect.start amplitude:effect.amplitude frequency:effect.frequency];
            break;
        case TriggerEffect::Mode::Off:
            [trigger setModeOff];
            break;
    }
}

}  // namespace

bool set_dualsense_triggers(int port, const TriggerEffect& effect) {
    if (@available(macOS 12.3, *)) {
        if (GCController* controller = port_dualsense(port)) {
            GCDualSenseGamepad* pad = (GCDualSenseGamepad*)controller.physicalInputProfile;
            apply(pad.leftTrigger, effect);
            apply(pad.rightTrigger, effect);
            return true;
        }
    }
    return false;
}

}  // namespace conker::input::apple
