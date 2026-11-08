// Haptic effects, and the rumble pak's rumble, on a DualSense through Apple's GameController
// framework and Core Haptics: one engine per grip of each port's pad, made the first time an effect
// plays on it and restarted when the system stops it. input_sdl.cpp calls these on the main thread.
#include "platform/haptics_apple.hpp"

#include <cstdio>
#include <string>

#import <CoreHaptics/CoreHaptics.h>
#import <GameController/GameController.h>

namespace conker::input::apple {

namespace {

// A port's engines, for the pad they were made for; remade when a different pad takes the port.
struct Engines {
    GCController* controller = nil;
    CHHapticEngine* left = nil;
    CHHapticEngine* right = nil;
    id<CHHapticPatternPlayer> left_rumble = nil;  // the rumble pak's, while it plays
    id<CHHapticPatternPlayer> right_rumble = nil;
    bool rumble_failed = false;
    RumbleFeel rumble_feel;  // the running rumble's  // Core Haptics refused the rumble: SDL rumbles the pad instead
};
Engines g_engines[kPorts];
bool g_reported = false;  // a failure is printed once, not for every effect

GCController* dualsense(int port) {
    GCController* controller = port_dualsense(port);
    return controller != nil && controller.haptics != nil ? controller : nil;
}

CHHapticEngine* make_engine(GCController* controller, GCHapticsLocality locality) {
    CHHapticEngine* engine = [controller.haptics createEngineWithLocality:locality];
    if (engine == nil) {
        if (!g_reported) {
            std::fprintf(stderr, "[conker] haptics: no Core Haptics engine for %s\n", locality.UTF8String);
        }
        return nil;
    }
    engine.playsHapticsOnly = YES;
    // Core Haptics stops an engine when the app is in the background or the server resets; start it
    // again for the next effect.
    __weak CHHapticEngine* weak = engine;
    engine.resetHandler = ^{
        [weak startAndReturnError:nil];
    };
    NSError* error = nil;
    if (![engine startAndReturnError:&error]) {
        if (!g_reported) {
            std::fprintf(stderr, "[conker] haptics: Core Haptics engine for %s did not start: %s\n",
                         locality.UTF8String, error.localizedDescription.UTF8String);
        }
        return nil;
    }
    return engine;
}

// The port's engines, made for the DualSense now in the port; nullptr when there are none.
Engines* engines_ready(int port) {
    if (port < 0 || port >= kPorts) {
        return nullptr;
    }
    Engines& engines = g_engines[port];
    GCController* controller = dualsense(port);
    if (controller == nil) {
        if (!g_reported) {
            std::fprintf(stderr, "[conker] haptics: no DualSense among the GameController framework's pads for controller %d\n",
                         port + 1);
            g_reported = true;
        }
        engines = Engines{};
        return nullptr;
    }
    if (controller != engines.controller || engines.left == nil || engines.right == nil) {
        engines = Engines{};
        engines.controller = controller;
        engines.left = make_engine(controller, GCHapticsLocalityLeftHandle);
        engines.right = make_engine(controller, GCHapticsLocalityRightHandle);
        if (engines.left != nil && engines.right != nil) {
            // Players, not names: a pad's vendorName is its Bluetooth name, often its owner's, and logs
            // go into bug reports.
            std::printf("[conker] haptics: controller %d: Core Haptics engines started on player %ld's DualSense "
                        "(DualSense pads as players:", port + 1, static_cast<long>(controller.playerIndex) + 1);
            for (GCController* pad in GCController.controllers) {
                if ([pad.physicalInputProfile isKindOfClass:[GCDualSenseGamepad class]]) {
                    std::printf(" %ld;", static_cast<long>(pad.playerIndex) + 1);
                }
            }
            std::printf(")\n");
        } else {
            g_reported = true;
        }
    }
    return engines.left != nil && engines.right != nil ? &engines : nullptr;
}

// The same effect feels stronger in the left grip than the right when it plays in one grip at a
// time (alternating L2 and R2 shots), so the left plays a one-grip effect this much weaker. In both
// grips at once the two sides play alike, as they should for something held in both hands.
constexpr float kLeftGripGain = 0.7f;

CHHapticPattern* pattern(const Haptic& haptic, float intensity) {
    NSArray<CHHapticEventParameter*>* parameters = @[
        [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity value:intensity],
        [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticSharpness value:haptic.sharpness],
    ];
    CHHapticEvent* event = haptic.duration > 0.0f
        ? [[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticContinuous parameters:parameters relativeTime:0
                                          duration:haptic.duration]
        : [[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticTransient parameters:parameters relativeTime:0];
    return [[CHHapticPattern alloc] initWithEvents:@[event] parameters:@[] error:nil];
}

void play_on(CHHapticEngine* engine, CHHapticPattern* pattern) {
    if (pattern == nil) {
        return;
    }
    NSError* error = nil;
    id<CHHapticPatternPlayer> player = [engine createPlayerWithPattern:pattern error:&error];
    if (player == nil) {
        return;
    }
    if (![player startAtTime:CHHapticTimeImmediate error:&error]) {
        // A stopped engine (stoppedHandler isn't called for every reason) starts again here.
        if ([engine startAndReturnError:nil]) {
            [player startAtTime:CHHapticTimeImmediate error:nil];
        }
    }
}

// The rumble pak's motor: the lowest sharpness, a deep rumble like a motor's, held until stopped.
constexpr float kRumbleSharpness = 0.0f;

id<CHHapticPatternPlayer> start_rumble(CHHapticEngine* engine, float intensity, float sharpness) {
    NSArray<CHHapticEventParameter*>* parameters = @[
        [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity value:intensity],
        [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticSharpness value:sharpness],
    ];
    CHHapticEvent* event = [[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticContinuous
                                                         parameters:parameters relativeTime:0
                                                           duration:GCHapticDurationInfinite];
    CHHapticPattern* rumble = [[CHHapticPattern alloc] initWithEvents:@[event] parameters:@[] error:nil];
    if (rumble == nil) {
        return nil;
    }
    id<CHHapticPatternPlayer> player = [engine createPlayerWithPattern:rumble error:nil];
    if (player == nil) {
        return nil;
    }
    if (![player startAtTime:CHHapticTimeImmediate error:nil]) {
        if (![engine startAndReturnError:nil] || ![player startAtTime:CHHapticTimeImmediate error:nil]) {
            return nil;
        }
    }
    return player;
}

void stop_rumble(__strong id<CHHapticPatternPlayer>& player) {
    if (player != nil) {
        [player stopAtTime:CHHapticTimeImmediate error:nil];
        player = nil;
    }
}

}  // namespace

GCController* port_dualsense(int port) {
    const std::string name = conker::input::controller_name(port);
    GCController* named = nil;
    GCController* numbered = nil;
    GCController* only = nil;
    int named_count = 0;
    int count = 0;
    for (GCController* controller in GCController.controllers) {
        if (![controller.physicalInputProfile isKindOfClass:[GCDualSenseGamepad class]]) {
            continue;
        }
        const bool has_name = controller.vendorName != nil && name == controller.vendorName.UTF8String;
        if (has_name) {
            named = controller;
            named_count++;
        }
        if (controller.playerIndex == static_cast<GCControllerPlayerIndex>(port) && (has_name || name.empty())) {
            numbered = controller;
        }
        only = controller;
        count++;
    }
    if (named_count == 1) {
        return named;
    }
    if (numbered != nil) {
        return numbered;
    }
    return port == 0 && count == 1 && named_count == 0 && name.empty() ? only : nil;
}

bool set_dualsense_rumble(int port, bool on, const RumbleFeel& feel) {
    if (port < 0 || port >= kPorts) {
        return false;
    }
    Engines& engines = g_engines[port];
    // Core Haptics raises an exception for what it can't do on a pad's engine (an advanced player,
    // for one); a rumble that won't play must not take the game down.
    @try {
        if (!on || !(feel == engines.rumble_feel)) {
            // Off, or another feel: a running rumble stops, to start again as asked.
            stop_rumble(engines.left_rumble);
            stop_rumble(engines.right_rumble);
            engines.rumble_feel = feel;
            if (!on) {
                return true;
            }
        }
        if (engines.rumble_failed || engines_ready(port) == nullptr) {
            return false;
        }
        if (engines.left_rumble == nil) {
            engines.left_rumble = start_rumble(engines.left, feel.intensity, feel.sharpness);
        }
        if (engines.right_rumble == nil) {
            engines.right_rumble = start_rumble(engines.right, feel.intensity, feel.sharpness);
        }
        engines.rumble_failed = engines.left_rumble == nil && engines.right_rumble == nil;
        return !engines.rumble_failed;
    } @catch (NSException* exception) {
        if (!g_reported) {
            std::fprintf(stderr, "[conker] haptics: controller %d rumble failed: %s\n", port + 1, exception.reason.UTF8String);
            g_reported = true;
        }
        engines.left_rumble = engines.right_rumble = nil;
        engines.rumble_failed = true;
        return false;
    }
}

bool play_dualsense_haptic(int port, const Haptic& haptic) {
    Engines* engines = engines_ready(port);
    if (engines == nullptr) {
        return false;
    }
    if (haptic.grip != Haptic::Grip::Right) {
        const float gain = haptic.grip == Haptic::Grip::Left ? kLeftGripGain : 1.0f;
        play_on(engines->left, pattern(haptic, haptic.intensity * gain));
    }
    if (haptic.grip != Haptic::Grip::Left) {
        play_on(engines->right, pattern(haptic, haptic.intensity));
    }
    return true;
}

}  // namespace conker::input::apple
