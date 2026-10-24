// DualSense adaptive triggers through Apple's GameController framework. On macOS SDL reads
// PlayStation pads through that framework (src/platform/input_sdl.cpp), and SDL can't send
// trigger effects that way, so input_sdl.cpp sets them here instead.
#include "platform/adaptive_triggers_apple.hpp"

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

bool set_dualsense_triggers(const TriggerEffect& effect) {
    if (@available(macOS 12.3, *)) {
        // Controller 1's DualSense: the one SDL numbered player 1 (input_sdl.cpp sets each pad's
        // player index to its port), or the only DualSense when none is numbered.
        GCDualSenseGamepad* only = nil;
        int count = 0;
        for (GCController* controller in GCController.controllers) {
            if (![controller.physicalInputProfile isKindOfClass:[GCDualSenseGamepad class]]) {
                continue;
            }
            GCDualSenseGamepad* pad = (GCDualSenseGamepad*)controller.physicalInputProfile;
            if (controller.playerIndex == GCControllerPlayerIndex1) {
                apply(pad.leftTrigger, effect);
                apply(pad.rightTrigger, effect);
                return true;
            }
            if (controller.playerIndex == GCControllerPlayerIndexUnset) {
                only = pad;
            }
            count++;
        }
        if (count == 1 && only != nil) {
            apply(only.leftTrigger, effect);
            apply(only.rightTrigger, effect);
            return true;
        }
    }
    return false;
}

}  // namespace conker::input::apple
