// Conker's black and white frames (the Tediz war briefing in scene 6, among others) in the RT64
// build. Native helpers for src/platform/renderer_rt64.cpp and the [[patches.hook]] snippet for
// func_150AF790 in config/us.toml.
//
// The game greys a frame on the CPU after the RDP has drawn it: the scheduler's swap
// (func_80005020) runs the colour-framebuffer effects (func_1515FDA0) on the frame it is about to
// show when that frame's scheduler task has flag 0x40, and effect type 2 (func_150AF790, table
// D_8008B0D8) calls func_150B1DB0, which copies each RGBA5551 pixel's green bits into red and blue
// over the whole 292x216 frame. RT64 draws the frame on the GPU and shows its own picture, so a
// change to the frame in RDRAM never reaches the screen, and the frame stayed in colour. Copies of
// it the game reads back, such as the pause menu's background, picked up the CPU's grey at the
// N64's resolution instead.
//
// Here the renderer asks RT64 to grey the frame's colour image after the last thing drawn to it
// (GreyFromGreen in tools/patches/rt64-grey-from-green.patch), at full resolution, and the CPU
// pass is skipped. Whether the game will grey a frame is decided when its display list is handed
// to RT64, by the same rules func_1515FDA0 applies at the swap.
#include "fixes/grey_frames.hpp"

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
constexpr int8_t kGreyType = 2;                    // D_8008B0D8[2] = func_150AF790
constexpr uint32_t kD_800C35EA = 0x800C35EA;

std::atomic<bool> g_gpu{false};
std::atomic<bool> g_cpu_pass_seen{false};
bool g_greying = false;

uint32_t read32(const uint8_t* rdram, uint32_t address) {
    return *reinterpret_cast<const uint32_t*>(rdram + (address & 0x3FFFFFC));
}

uint8_t read8(const uint8_t* rdram, uint32_t address) {
    return rdram[(address & 0x3FFFFFF) ^ 3];
}

// Whether func_1515FDA0 will run a grey effect at the next swap.
bool grey_effect_due(const uint8_t* rdram) {
    const bool cpu_pass_seen = g_cpu_pass_seen.exchange(false);
    for (uint32_t group = 0; group < 2; group++) {
        uint32_t effect = read32(rdram, kEffectGroups + group * kEffectGroupStride);
        for (int guard = 0; effect != 0 && guard < 256; guard++, effect = read32(rdram, effect + kEffectNext)) {
            if (static_cast<int8_t>(read8(rdram, effect + kEffectType)) != kGreyType) {
                continue;
            }
            const uint8_t flags = read8(rdram, effect + kEffectFlags);
            if (flags & 4) {
                continue;  // the swap clears the bit and skips the effect this once
            }
            if ((flags & 2) && read8(rdram, kD_800C35EA) == 1) {
                continue;
            }
            if ((flags & 8) && !cpu_pass_seen) {
                continue;  // decided by game functions at the swap: follow the last frame's outcome
            }
            return true;
        }
    }
    return false;
}

}  // namespace

void conker::grey_frames::use_gpu(bool on) { g_gpu = on; }

uint32_t conker::grey_frames::frame_to_grey(const uint8_t* rdram, uint32_t data_ptr) {
    uint32_t result = 0;
    const uint32_t task = read32(rdram, kCurrentTask);
    if (task != 0 && read8(rdram, kEffectsHeld) == 0 && (read32(rdram, task + kTaskFlags) & kRunEffects) &&
        (read32(rdram, task + kTaskDataPtr) & 0x3FFFFFF) == (data_ptr & 0x3FFFFFF) && grey_effect_due(rdram)) {
        result = read32(rdram, task + kTaskFramebuffer) & 0xFFFFFF;
    }
    const bool greying = result != 0;
    if (greying != g_greying) {
        g_greying = greying;
        std::printf("[conker] black and white frames %s\n", greying ? "on" : "off");
    }
    return result;
}

// At func_150AF790's entry, the CPU pass of the grey effect: nonzero to return without it, when
// RT64 greys the frame instead.
extern "C" int conker_grey_cpu_pass() {
    g_cpu_pass_seen = true;
    return g_gpu ? 1 : 0;
}
