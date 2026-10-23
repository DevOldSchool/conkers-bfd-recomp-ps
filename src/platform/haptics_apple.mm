// Haptic effects on a DualSense through Apple's GameController framework and Core Haptics: one
// engine per grip, made the first time an effect plays on a pad and restarted when the system
// stops it. input_sdl.cpp calls these on the main thread.
#include "platform/haptics_apple.hpp"

#include <cstdio>

#import <CoreHaptics/CoreHaptics.h>
#import <GameController/GameController.h>

namespace conker::input::apple {

namespace {

// The engines for the pad they were made for; remade when a different pad is connected.
GCController* g_controller = nil;
CHHapticEngine* g_left = nil;
CHHapticEngine* g_right = nil;
bool g_reported = false;  // a failure is printed once, not for every effect

GCController* dualsense() {
    for (GCController* controller in GCController.controllers) {
        if ([controller.physicalInputProfile isKindOfClass:[GCDualSenseGamepad class]] && controller.haptics != nil) {
            return controller;
        }
    }
    return nil;
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

bool engines_ready() {
    GCController* controller = dualsense();
    if (controller == nil) {
        if (!g_reported) {
            std::fprintf(stderr, "[conker] haptics: no DualSense among the GameController framework's pads\n");
            g_reported = true;
        }
        g_controller = nil;
        g_left = g_right = nil;
        return false;
    }
    if (controller != g_controller || g_left == nil || g_right == nil) {
        g_controller = controller;
        g_left = make_engine(controller, GCHapticsLocalityLeftHandle);
        g_right = make_engine(controller, GCHapticsLocalityRightHandle);
        if (g_left != nil && g_right != nil) {
            std::printf("[conker] haptics: Core Haptics engines started on %s's grips\n", controller.vendorName.UTF8String);
        } else {
            g_reported = true;
        }
    }
    return g_left != nil && g_right != nil;
}

void play_on(CHHapticEngine* engine, CHHapticPattern* pattern) {
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

}  // namespace

bool play_dualsense_haptic(const Haptic& haptic) {
    if (!engines_ready()) {
        return false;
    }
    NSArray<CHHapticEventParameter*>* parameters = @[
        [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity value:haptic.intensity],
        [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticSharpness value:haptic.sharpness],
    ];
    CHHapticEvent* event = haptic.duration > 0.0f
        ? [[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticContinuous parameters:parameters relativeTime:0
                                          duration:haptic.duration]
        : [[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticTransient parameters:parameters relativeTime:0];
    CHHapticPattern* pattern = [[CHHapticPattern alloc] initWithEvents:@[event] parameters:@[] error:nil];
    if (pattern == nil) {
        return false;
    }
    if (haptic.grip != Haptic::Grip::Right) {
        play_on(g_left, pattern);
    }
    if (haptic.grip != Haptic::Grip::Left) {
        play_on(g_right, pattern);
    }
    return true;
}

}  // namespace conker::input::apple
