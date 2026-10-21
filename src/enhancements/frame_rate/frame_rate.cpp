// Experimental 60 fps: CONKER_60FPS=1 lets Rare's scheduler start a frame on every retrace instead
// of every second one. Off by default; docs/investigations.md has what was measured. Native helpers
// called from the [[patches.hook]] snippets for func_1501C880, func_800049E0, func_80004DB0,
// func_80005020, and the half steps in func_15116BAC, func_151169B4 and func_8000BF60, in
// config/us.toml.
//
// The game logic already steps by the retraces its frame took: func_80004F00 copies the count since
// the last frame started (D_8003B238) to D_800BE9E4, and the logic scales by it (D_800BE9A4 =
// count * 0.5). What holds it to 30 fps is the scheduler thread (func_800049E0), which looks at the
// game's task queue only on a retrace. A recompiled frame takes about a millisecond, so it is done
// before the next retrace, waits for it to swap (state 6), and the frame after it is then picked up
// one retrace later still: always 2 retraces. Here the scheduler is woken when a frame is swapped
// and when the game queues its next one, so the next frame starts in the retrace its predecessor
// was swapped in, and the count reads 1.
//
// CONKER_FRAME_LOG=<file> (either mode) writes a line per frame: VI, the frame's retrace count,
// scene, logic delta (D_800BE9A4), Conker's position and the camera's eye, for comparing the two
// rates.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "recomp.h"
#include "ultramodern/ultra64.h"
#include "enhancements/status.hpp"
#include "runtime/game.hpp"

extern "C" void func_80004DB0(uint8_t* rdram, recomp_context* ctx);

namespace {

inline gpr kseg0(uint32_t address) { return static_cast<gpr>(static_cast<int32_t>(address)); }

constexpr uint32_t kSchedulerQueue = 0x8003B218;  // D_8003B218: the scheduler thread's messages
constexpr uint32_t kAudioTaskRunning = 0x8003A581;  // D_8003A581
// func_800049E0's jump table covers messages 0 to 6 and ignores the rest; 7 is free.
constexpr int32_t kWakeMessage = 7;
constexpr int kMessageSlot = 0x64;  // func_800049E0 receives into sp+0x64
constexpr uint32_t kFrameTicks = 0x800BE9E4;    // D_800BE9E4: retraces this frame steps by
constexpr uint32_t kHalfSteps = 0x800BE9A0;     // D_800BE9A0: 30 Hz steps this frame (u8), 0 or 1 at 60 fps
constexpr uint32_t kFrameDelta = 0x800BE9A4;    // D_800BE9A4: the same as a float, 1.0 = 1/30 s
constexpr uint32_t kScene = 0x800BE9F0;         // D_800BE9F0
constexpr uint32_t kPlayerObject = 0x80088720;  // D_80088720: Conker's object, position at +0x14

bool enabled() {
    static const bool on = [] {
        const char* value = std::getenv("CONKER_60FPS");
        return value != nullptr && value[0] == '1';
    }();
    return on;
}

void wake_scheduler(uint8_t* rdram) {
    osSendMesg(rdram, static_cast<int32_t>(kSchedulerQueue), kWakeMessage, OS_MESG_NOBLOCK);
}

float read_float(uint8_t* rdram, uint32_t address) {
    const int32_t bits = MEM_W(0, kseg0(address));
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void log_frame(uint8_t* rdram) {
    static FILE* file = [] {
        const char* path = std::getenv("CONKER_FRAME_LOG");
        FILE* opened = path != nullptr && path[0] != '\0' ? std::fopen(path, "w") : nullptr;
        if (opened != nullptr) {
            std::setvbuf(opened, nullptr, _IOLBF, 0);  // runs end by being killed
        }
        return opened;
    }();
    if (file == nullptr) {
        return;
    }
    const uint32_t object = static_cast<uint32_t>(MEM_W(0, kseg0(kPlayerObject)));
    float position[3] = {}, eye[3] = {};
    if (object >= 0x80000000u && object < 0x80800000u) {
        const uint32_t camera = static_cast<uint32_t>(MEM_W(0, kseg0(object + 0x31C)));
        for (int i = 0; i < 3; i++) {
            position[i] = read_float(rdram, object + 0x14 + 4 * i);
            if (camera >= 0x80000000u && camera < 0x80800000u) {
                eye[i] = read_float(rdram, camera + 0x13C + 4 * i);
            }
        }
    }
    std::fprintf(file, "%llu %d %x %.3f %.3f %.3f %.3f %.3f %.3f %.3f\n",
                 static_cast<unsigned long long>(conker::current_vi()), static_cast<int>(MEM_W(0, kseg0(kFrameTicks))),
                 static_cast<unsigned>(MEM_W(0, kseg0(kScene))), read_float(rdram, kFrameDelta), position[0],
                 position[1], position[2], eye[0], eye[1], eye[2]);
}

}  // namespace

// func_1501C880, after it queues a frame's task on D_8003B1E8.
extern "C" void conker_60fps_task_queued(uint8_t* rdram, recomp_context* /*ctx*/) {
    log_frame(rdram);
    if (enabled()) {
        wake_scheduler(rdram);
    }
}

// func_80005020, after it swaps a frame in and tells the game the frame is done. The next frame may
// already be queued.
extern "C" void conker_60fps_swapped(uint8_t* rdram, recomp_context* /*ctx*/) {
    if (enabled()) {
        wake_scheduler(rdram);
    }
}

// func_800049E0, after each message: on a wake, try to start the queued frame, as a retrace would
// (it too waits while an audio task runs).
extern "C" void conker_60fps_scheduler_message(uint8_t* rdram, recomp_context* ctx) {
    if (static_cast<int32_t>(MEM_W(kMessageSlot, ctx->r29)) != kWakeMessage) {
        return;
    }
    if (MEM_BU(0, kseg0(kAudioTaskRunning)) == 0) {
        func_80004DB0(rdram, ctx);
    }
}

// Integer steps written as (rate * ticks) >> 1, which lose the odd half: at one retrace a frame, a
// rate of 1 never moves and 3 moves at two thirds of its speed. Called before the shift with the
// product in a register; at one retrace it becomes rate * 2 * D_800BE9A0, so the step is the rate
// on every second frame, as at 30 fps. func_15116BAC and func_151169B4 move an object's s16
// position toward a target (with a sound, so doors and platforms); func_8000BF60 counts down the
// title scene's fade of sound 0x22, which never reached 0.
extern "C" void conker_60fps_half_step(uint8_t* rdram, gpr* product) {
    if (enabled() && MEM_W(0, kseg0(kFrameTicks)) == 1) {
        *product = static_cast<gpr>(static_cast<int32_t>(*product) * 2 * MEM_BU(0, kseg0(kHalfSteps)));
    }
}

// func_80004DB0 starts a frame only if its framebuffer is neither the one the VI shows now
// (osViGetCurrentFramebuffer, $v0 here) nor the one it will show next. There are two framebuffers,
// so a frame that starts in the retrace its predecessor was swapped in draws into the one still on
// screen. RT64 does not scan RDRAM out, so only the "next" check is kept.
extern "C" void conker_60fps_current_framebuffer(uint8_t* /*rdram*/, recomp_context* ctx) {
    if (enabled()) {
        ctx->r2 = 0;
    }
}

nlohmann::json conker::status::frame_rate() {
    return enhancement("frame_rate", "60 fps (experimental)", enabled(), "CONKER_60FPS");
}
