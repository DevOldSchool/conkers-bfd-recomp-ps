// Conker's colour-framebuffer effects in the RT64 build: black and white frames (the Tediz war
// briefing in scene 6, among others), the mosaic that censors the caveman in scene 20's cutscene 21,
// and the lens in the middle of the sight when aiming. Native helpers for
// src/platform/renderer_rt64.cpp and the [[patches.hook]] snippets for func_150AF790, func_150CE450
// and func_151CB110 in config/us.toml.
//
// The game makes these effects on the CPU after the RDP has drawn the frame: the scheduler's swap
// (func_80005020) runs the colour-framebuffer effects (func_1515FDA0) on the frame it is about to
// show when that frame's scheduler task has flag 0x40, through D_8008B0D8 by effect type:
// - 2, func_150AF790: func_150B1DB0 copies each RGBA5551 pixel's green bits into red and blue over
//   the whole 292x216 frame;
// - 0, func_150CE450: a mosaic over a rectangle that func_150CE200 keeps on a character: each pixel
//   takes a source pixel that steps on every 0x10000 / step pixels, across and down;
// - 1, func_151CB110: a lens 24 pixels across in the middle of a player's view, magnifying up to
//   3x towards its middle, drawn from a copy of the 24x24 square around it.
// RT64 draws the frame on the GPU and shows its own picture, so a change to the frame in RDRAM never
// reaches the screen, and none of them showed. Copies of the frame the game reads back, such as the
// pause menu's background, picked them up at the N64's resolution instead.
//
// Here the renderer asks RT64 to run each on the frame's colour image after the last thing drawn to
// it (GreyFromGreen in tools/patches/rt64-grey-from-green.patch, FrameMosaic and FrameLens in
// tools/patches/rt64-frame-effects.patch), at full resolution, and the CPU passes are skipped.
// Which effects the game will run on a frame is decided when its display list is handed to RT64,
// by the same rules func_1515FDA0 and the effects apply at the swap.
#include "fixes/frame_effects.hpp"

#include <atomic>
#include <cstdio>

#include "recomp.h"

namespace {

constexpr uint32_t kCurrentTask = 0x8002AC50;      // D_8002AC50: the scheduler task being run
constexpr uint32_t kEffectsHeld = 0x8002AC5C;      // D_8002AC5C: nonzero skips the effects
constexpr uint32_t kTaskFlags = 0xC;               // u32, 0x40: run the colour-framebuffer effects
constexpr uint32_t kTaskFramebuffer = 0x10;
constexpr uint32_t kTaskDataPtr = 0x18 + 0x30;     // the OSTask's data_ptr
constexpr uint32_t kRunEffects = 0x40;
constexpr uint32_t kEffectGroups = 0x800DCF20;     // D_800DCF20: two lists, 0x1A0 apart
constexpr uint32_t kEffectGroupStride = 0x1A0;
constexpr uint32_t kEffectNext = 0x8;
constexpr uint32_t kEffectFlags = 0xE;             // payload[0]
constexpr uint32_t kEffectType = 0x10;             // payload[2], s8, -1 for none
constexpr uint32_t kD_800C35EA = 0x800C35EA;
constexpr uint32_t kFrameWidth = 0x800BE620;       // D_800BE620: the frame's width in pixels

// Type 0, func_150CE450: the GameFB600Remap at +0x18.
constexpr uint32_t kMosaicStep = 0x28;             // u32, 16.16
constexpr uint32_t kMosaicFlags = 0x2C;            // u8, 1: on screen this frame (func_150CE200)
constexpr uint32_t kMosaicLeft = 0x30;             // u32 left, top, right, bottom, inclusive
constexpr uint32_t kMosaicTop = 0x34;
constexpr uint32_t kMosaicRight = 0x38;
constexpr uint32_t kMosaicBottom = 0x3C;

// Type 1, func_151CB110: the SightWarpState at +0x18.
constexpr uint32_t kLensView = 0x1C;               // the player's view (D_800BE628 entry)
constexpr uint32_t kLensColumn = 0x24;             // u32: left of the copied square
constexpr uint32_t kLensRowOffset = 0x28;          // u32: its top times the frame's width
constexpr uint32_t kViewCenterX = 0x34;            // f32, the view's middle in pixels
constexpr uint32_t kViewCenterY = 0x38;
constexpr uint32_t kWorldPaused = 0x800BEAC0;      // D_800BEAC0..2: nonzero skips the lens
constexpr float kLensRadius = 12.0f;
constexpr uint32_t kLensStrength = 0x800AAE9C;     // D_800AAE9C, f32: 0.67, 3x in the middle

std::atomic<bool> g_gpu{false};
std::atomic<bool> g_cpu_pass_seen[3];
bool g_logged[3] = {};

uint32_t read32(const uint8_t* rdram, uint32_t address) {
    return *reinterpret_cast<const uint32_t*>(rdram + (address & 0x3FFFFFC));
}

float read_float(const uint8_t* rdram, uint32_t address) {
    return *reinterpret_cast<const float*>(rdram + (address & 0x3FFFFFC));
}

uint8_t read8(const uint8_t* rdram, uint32_t address) {
    return rdram[(address & 0x3FFFFFF) ^ 3];
}

void log_change(int type, bool on) {
    static const char* const kNames[] = {"mosaic", "sight lens", "black and white frames"};
    if (g_logged[type] != on) {
        g_logged[type] = on;
        std::printf("[conker] %s %s\n", kNames[type], on ? "on" : "off");
        std::fflush(stdout);
    }
}

// The effect at `effect` as the renderer is to run it, if the game will run it on the frame at
// the next swap, by func_1515FDA0's rules and the effect's own.
bool effect_due(const uint8_t* rdram, uint32_t effect, int8_t type, const bool cpu_pass_seen[3],
                conker::frame_effects::Effect* out) {
    const uint8_t flags = read8(rdram, effect + kEffectFlags);
    if (flags & 4) {
        return false;  // the swap clears the bit and skips the effect this once
    }
    if ((flags & 2) && read8(rdram, kD_800C35EA) == 1) {
        return false;
    }
    if ((flags & 8) && !cpu_pass_seen[type]) {
        return false;  // decided by game functions at the swap: follow the last frame's outcome
    }
    out->type = static_cast<conker::frame_effects::Type>(type);
    if (type == 0) {
        if (!(read8(rdram, effect + kMosaicFlags) & 1)) {
            return false;
        }
        const uint32_t left = read32(rdram, effect + kMosaicLeft);
        const uint32_t top = read32(rdram, effect + kMosaicTop);
        const uint32_t right = read32(rdram, effect + kMosaicRight);
        const uint32_t bottom = read32(rdram, effect + kMosaicBottom);
        if (left > right || top > bottom || right > 0xFFFF || bottom > 0xFFFF) {
            return false;
        }
        out->mosaic = {static_cast<uint16_t>(left), static_cast<uint16_t>(top), static_cast<uint16_t>(right),
                       static_cast<uint16_t>(bottom), read32(rdram, effect + kMosaicStep)};
    } else if (type == 1) {
        if (read8(rdram, kWorldPaused) || read8(rdram, kWorldPaused + 1) || read8(rdram, kWorldPaused + 2)) {
            return false;
        }
        const uint32_t view = read32(rdram, effect + kLensView);
        const uint32_t width = read32(rdram, kFrameWidth);
        if (view == 0 || width == 0) {
            return false;
        }
        // The lens is drawn round the view's middle now, from the square copied where it was when
        // the effect was made (the same place for one player).
        out->lens = {read_float(rdram, view + kViewCenterX), read_float(rdram, view + kViewCenterY),
                     static_cast<float>(read32(rdram, effect + kLensColumn)) + kLensRadius,
                     static_cast<float>(read32(rdram, effect + kLensRowOffset) / width) + kLensRadius, kLensRadius,
                     read_float(rdram, kLensStrength)};
    }
    return true;
}

}  // namespace

void conker::frame_effects::use_gpu(bool on) { g_gpu = on; }

uint32_t conker::frame_effects::frame_effects(const uint8_t* rdram, uint32_t data_ptr, std::vector<Effect>* effects) {
    effects->clear();
    bool cpu_pass_seen[3];
    for (int type = 0; type < 3; type++) {
        cpu_pass_seen[type] = g_cpu_pass_seen[type].exchange(false);
    }
    uint32_t frame = 0;
    const uint32_t task = read32(rdram, kCurrentTask);
    if (task != 0 && read8(rdram, kEffectsHeld) == 0 && (read32(rdram, task + kTaskFlags) & kRunEffects) &&
        (read32(rdram, task + kTaskDataPtr) & 0x3FFFFFF) == (data_ptr & 0x3FFFFFF)) {
        for (uint32_t group = 0; group < 2; group++) {
            uint32_t effect = read32(rdram, kEffectGroups + group * kEffectGroupStride);
            for (int guard = 0; effect != 0 && guard < 256; guard++, effect = read32(rdram, effect + kEffectNext)) {
                const int8_t type = static_cast<int8_t>(read8(rdram, effect + kEffectType));
                Effect due;
                if (type >= 0 && type <= 2 && effect_due(rdram, effect, type, cpu_pass_seen, &due)) {
                    effects->push_back(due);
                }
            }
        }
        if (!effects->empty()) {
            frame = read32(rdram, task + kTaskFramebuffer) & 0xFFFFFF;
        }
    }
    bool on[3] = {};
    for (const Effect& effect : *effects) {
        on[static_cast<int>(effect.type)] = true;
    }
    for (int type = 0; type < 3; type++) {
        log_change(type, on[type]);
    }
    return frame;
}

// At the entry of an effect's CPU pass (type: 0 mosaic, 1 lens, 2 grey): nonzero to return without
// it, when RT64 makes the effect instead.
extern "C" int conker_frame_effect_cpu_pass(int type) {
    if (type >= 0 && type < 3) {
        g_cpu_pass_seen[type] = true;
    }
    return g_gpu ? 1 : 0;
}
