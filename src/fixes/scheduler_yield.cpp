// Keeps Rare's scheduler from taking a second audio task while it waits for a graphics task to
// yield. Native helper called from the [[patches.hook]] snippet for func_800049E0 in
// config/us.toml.
#include <cstdint>

#include "recomp.h"

namespace {

inline gpr kseg0(uint32_t address) { return static_cast<gpr>(static_cast<int32_t>(address)); }

constexpr uint32_t kTimerPending = 0x8002AC6C;  // D_8002AC6C: the audio start timer is set (s8)
constexpr uint32_t kState = 0x8003A582;         // D_8003A582: graphics state, 3 = yield asked for
constexpr uint8_t kYieldAsked = 3;

bool g_held = false;  // scheduler thread only

}  // namespace

// func_800049E0 takes the next audio task (D_8002AC54) on a retrace when no audio task runs and its
// start timer is not set (D_8002AC6C), and sets that timer. When the timer fires while a graphics
// task runs, it asks the RSP to yield and waits in state 3; the SP completion that follows starts
// the audio task in D_8002AC54. The timer message has cleared D_8002AC6C by then, so a retrace in
// state 3 takes the following audio task into D_8002AC54 and sets a second timer. On hardware the
// yield takes microseconds and no retrace falls in it. Here yields are ignored and the graphics
// task's SP completion comes when the gfx thread takes the task, which under CPU load can be
// several retraces later. The first audio task is then lost and the second one is started twice;
// its second SP completion is taken for a graphics one and completes the current frame a second
// time (func_80005020). The game's pending-frame count (D_800BEAA8) drops below the frames really
// queued, so at the next scene change it frees its display lists and builds the new scene's first
// frames into them while RT64 is still drawing the old scene's last frame from the same memory.
// Called after each message, before it is handled: keep the timer marked as set while a yield is
// waiting, so the retrace leaves D_8002AC54 alone, and clear it once the audio task has started.
extern "C" void conker_scheduler_yield_guard(uint8_t* rdram, recomp_context* /*ctx*/) {
    if (MEM_BU(0, kseg0(kState)) == kYieldAsked) {
        MEM_B(0, kseg0(kTimerPending)) = 1;
        g_held = true;
    } else if (g_held) {
        MEM_B(0, kseg0(kTimerPending)) = 0;
        g_held = false;
    }
}
