// Haptic effects, and the rumble pak's rumble, on a DualSense through Apple's GameController
// framework and Core Haptics: one engine per grip, made the first time an effect plays on a pad and
// restarted when the system stops it. input_sdl.cpp calls these on the main thread.
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
id<CHHapticPatternPlayer> g_left_rumble = nil;   // the rumble pak's, while it plays
id<CHHapticPatternPlayer> g_right_rumble = nil;
bool g_rumble_failed = false;  // Core Haptics refused the rumble: SDL rumbles the pad instead
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
        g_left_rumble = g_right_rumble = nil;
        return false;
    }
    if (controller != g_controller || g_left == nil || g_right == nil) {
        g_controller = controller;
        g_left_rumble = g_right_rumble = nil;
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

id<CHHapticPatternPlayer> start_rumble(CHHapticEngine* engine, float intensity) {
    NSArray<CHHapticEventParameter*>* parameters = @[
        [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity value:intensity],
        [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticSharpness value:kRumbleSharpness],
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

bool set_dualsense_rumble(bool on) {
    // Core Haptics raises an exception for what it can't do on a pad's engine (an advanced player,
    // for one); a rumble that won't play must not take the game down.
    @try {
        if (!on) {
            stop_rumble(g_left_rumble);
            stop_rumble(g_right_rumble);
            return true;
        }
        if (g_rumble_failed || !engines_ready()) {
            return false;
        }
        if (g_left_rumble == nil) {
            g_left_rumble = start_rumble(g_left, 1.0f);
        }
        if (g_right_rumble == nil) {
            g_right_rumble = start_rumble(g_right, 1.0f);
        }
        g_rumble_failed = g_left_rumble == nil && g_right_rumble == nil;
        return !g_rumble_failed;
    } @catch (NSException* exception) {
        if (!g_reported) {
            std::fprintf(stderr, "[conker] haptics: rumble failed: %s\n", exception.reason.UTF8String);
            g_reported = true;
        }
        g_left_rumble = g_right_rumble = nil;
        g_rumble_failed = true;
        return false;
    }
}

bool play_dualsense_haptic(const Haptic& haptic) {
    if (!engines_ready()) {
        return false;
    }
    if (haptic.grip != Haptic::Grip::Right) {
        const float gain = haptic.grip == Haptic::Grip::Left ? kLeftGripGain : 1.0f;
        play_on(g_left, pattern(haptic, haptic.intensity * gain));
    }
    if (haptic.grip != Haptic::Grip::Left) {
        play_on(g_right, pattern(haptic, haptic.intensity));
    }
    return true;
}

}  // namespace conker::input::apple
