// With CONKER_FIXED_STEP=1, the audio frames are made by the game thread at the start of each game
// frame instead of by the audio thread on the real clock (docs/online-multiplayer-plan.md, M1).
// Native helpers called from the [[patches.hook]] snippets for func_80009400 and func_800095A0 in
// config/us.toml, and from the frame input hook.
//
// The audio thread (func_80009400) makes an audio frame (func_800095A0) on every second retrace
// message from the scheduler, then waits for the previous frame's RSP task. Making a frame steps
// the sound players: sounds end, and the game frees what they held, and the audio thread frees
// game heap memory itself. When that happens relative to the game thread's frames depended on
// the real clock, and so did the frame's length, chosen from how much audio was still queued
// (AI_LEN). Two runs of the same input then put the same allocations at different addresses from
// the first sound that ended a frame apart, and differed everywhere after.
//
// Here the audio thread ignores its retrace messages, and the game thread makes the audio frame
// itself, the same way, at the start of every game frame (every second one at 60 fps), so it
// lands at the same point of the game's frames in every run. The queued audio it chooses the
// length by is a model instead of the real queue: what the frames made, less 1/30 s at the
// output rate a frame. It averages out to the real rate, as Rare's choice does.
#include <cstdint>
#include <deque>

#include <cstring>
#include <vector>

#include "recomp.h"
#include "enhancements/frame_rate/frame_rate.hpp"
#include "runtime/frame_input.hpp"
#include "runtime/snapshot.hpp"

extern "C" void func_800095A0(uint8_t* rdram, recomp_context* ctx);
extern "C" void osRecvMesg_recomp(uint8_t* rdram, recomp_context* ctx);
extern uint8_t dmem[];  // librecomp rsp.cpp

namespace {

inline gpr kseg0(uint32_t address) { return static_cast<gpr>(static_cast<int32_t>(address)); }

constexpr uint32_t kAudioStarted = 0x8002AE40;     // D_8002AE40 (u8): the audio thread is running
constexpr uint32_t kShuttingDown = 0x8002AC5C;     // D_8002AC5C (u8)
constexpr uint32_t kAudioFrameCount = 0x8002AE44;  // D_8002AE44: audio frames made
constexpr uint32_t kTaskRecords = 0x8003E390;      // D_8003E390: three AudioTaskRecord pointers
constexpr uint32_t kCompletionQueue = 0x8003E608;  // D_8003E608: the RSP's "audio task done" messages
constexpr uint32_t kShortFrame = 0x80040F88;       // D_80040F88: samples in a short audio frame
constexpr uint32_t kLongFrame = 0x80040F8C;        // D_80040F8C: in a long one
constexpr uint32_t kShortCooldown = 0x80040F84;    // D_80040F84: frames before another short one
constexpr int32_t kRetraceMessage = 1;
constexpr uint32_t kFullAt = 0xF9;                 // func_800095A0: samples queued that make a short frame
constexpr uint32_t kSamplesPerFrame = 22020 / 30;  // the game's output rate over 30 audio frames a second

uint32_t g_previous = 0;    // the AudioTaskRecord of the frame before, as the audio thread keeps it
bool g_first = true;
uint32_t g_in_flight = 0;          // audio tasks made whose "done" has not been taken yet
std::deque<uint32_t> g_finished;   // AudioTaskRecords of finished tasks, not yet handed out
constexpr uint32_t kGameQueueCount = 0x800BEA18;  // the game thread's queue (D_800BEA10): messages
constexpr uint32_t kGameQueueSize = 0x800BEA20;   // and slots
uint32_t g_calls = 0;
int64_t g_queued = 0;       // samples the model has queued
bool in_rdram(uint32_t address) { return address >= 0x80000000u && address < 0x80800000u; }
// Native state the game's memory depends on, carried by a snapshot (runtime/snapshot.hpp).
const bool g_snapshot_fields = [] {
    // Checked on load, as another machine's word: the records are RDRAM addresses the game
    // reads through, and a task count other than 0 made the game thread wait for tasks never sent.
    conker::snapshot::add("frame_audio.previous", &g_previous, sizeof(g_previous), [](const uint8_t* bytes) {
        uint32_t value;
        std::memcpy(&value, bytes, sizeof(value));
        return value == 0 || in_rdram(value);
    });
    conker::snapshot::add_bools("frame_audio.first", &g_first, 1);
    conker::snapshot::add("frame_audio.in_flight", &g_in_flight, sizeof(g_in_flight), [](const uint8_t* bytes) {
        uint32_t value;
        std::memcpy(&value, bytes, sizeof(value));
        return value == 0;  // a snapshot is taken only with no task out (the check below)
    });
    conker::snapshot::add("frame_audio.calls", &g_calls, sizeof(g_calls));
    conker::snapshot::add("frame_audio.queued", &g_queued, sizeof(g_queued), [](const uint8_t* bytes) {
        int64_t value;
        std::memcpy(&value, bytes, sizeof(value));
        return value >= 0 && value <= 0x100000;
    });
    conker::snapshot::add_check([] { return g_in_flight != 0 ? "an audio task is still out" : nullptr; });
    // The RSP's data memory, which keeps what the audio microcode leaves there from one task to the next.
    conker::snapshot::add("rsp.dmem", dmem, 0x1000);
    conker::snapshot::add(
        "frame_audio.finished",
        [] {
            std::vector<uint8_t> out(g_finished.size() * 4);
            for (size_t i = 0; i < g_finished.size(); i++) std::memcpy(&out[i * 4], &g_finished[i], 4);
            return out;
        },
        [](const std::vector<uint8_t>& in) {
            // Three task records at most (the audio thread keeps three), each in RDRAM.
            if (in.size() % 4 != 0 || in.size() > 3 * 4) return false;
            for (size_t i = 0; i < in.size(); i += 4) {
                uint32_t v;
                std::memcpy(&v, &in[i], 4);
                if (!in_rdram(v)) return false;
            }
            g_finished.clear();
            for (size_t i = 0; i < in.size(); i += 4) {
                uint32_t v;
                std::memcpy(&v, &in[i], 4);
                g_finished.push_back(v);
            }
            return true;
        });
    return true;
}();

// Waits for the oldest audio task's "done" and keeps its AudioTaskRecord.
void receive_done(uint8_t* rdram, recomp_context& ctx, const recomp_context* hook_ctx, gpr message) {
    ctx.r29 = hook_ctx->r29 - 0x80;
    ctx.r4 = kseg0(kCompletionQueue);
    ctx.r5 = message;
    ctx.r6 = 1;  // OS_MESG_BLOCK
    osRecvMesg_recomp(rdram, &ctx);
    const uint32_t completion = static_cast<uint32_t>(MEM_W(0, message));
    g_finished.push_back(static_cast<uint32_t>(MEM_W(4, kseg0(completion))));
    g_in_flight--;
}

}  // namespace

// func_80009400, with the message type it just received in $v0: under the fixed step a retrace
// message does nothing, as the game thread makes the audio frames.
extern "C" void conker_audio_thread_message(uint8_t* /*rdram*/, recomp_context* ctx) {
    if (conker::frame_input::fixed_step() && static_cast<int16_t>(ctx->r2) == kRetraceMessage) {
        ctx->r2 = 0;
    }
}

// func_800095A0, where it has read the audio still queued (bytes, in $v1): under the fixed step,
// the model's instead, and the model counts the frame this choice makes.
extern "C" void conker_audio_queued(uint8_t* rdram, recomp_context* ctx) {
    if (!conker::frame_input::fixed_step()) {
        return;
    }
    g_queued = g_queued > kSamplesPerFrame ? g_queued - kSamplesPerFrame : 0;
    const bool short_frame = g_queued >= kFullAt && MEM_W(0, kseg0(kShortCooldown)) == 0;
    ctx->r3 = static_cast<gpr>(static_cast<int32_t>(g_queued * 4));
    g_queued += MEM_W(0, kseg0(short_frame ? kShortFrame : kLongFrame));
}

// At the start of each game frame, on the game thread: the audio frame, as func_80009400 makes it.
void conker::frame_input::make_audio_frame(uint8_t* rdram, recomp_context* hook_ctx) {
    if (!fixed_step() || MEM_BU(0, kseg0(kAudioStarted)) == 0 || MEM_BU(0, kseg0(kShuttingDown)) != 0) {
        return;
    }
    // Two retraces an audio frame: every game frame, or every second one at 60 fps.
    if (frame_rate::sixty_fps() && (g_calls++ & 1) != 0) {
        return;
    }
    // A copy of the hook's registers, with a stack frame below the hook's for the calls.
    recomp_context ctx = *hook_ctx;
    ctx.r29 = hook_ctx->r29 - 0x80;
    const gpr message = ctx.r29 + 0x78;  // an OSMesg slot in the space below the hook's frame
    const uint32_t slot = static_cast<uint32_t>(MEM_W(0, kseg0(kAudioFrameCount))) % 3;
    ctx.r4 = kseg0(static_cast<uint32_t>(MEM_W(4 * slot, kseg0(kTaskRecords))));
    ctx.r5 = kseg0(g_previous);
    func_800095A0(rdram, &ctx);
    if (static_cast<int32_t>(ctx.r2) == 0) {
        return;
    }
    // The audio thread takes the oldest task's "done" after making the next frame, so the task
    // just made is still writing its samples into RDRAM while the game's frame runs. Here the
    // task is waited for at once, so its output is in place by the frame's state hash, and the
    // records are handed out a frame later, in the order the audio thread would take them.
    //
    // Rare's scheduler starts the task, and it blocks sending a "task done" to the game thread's
    // queue when that is full (of retrace messages, after a long wait for RT64); then the game must
    // not wait here, and the "done" is taken on a later frame.
    g_in_flight++;
    const bool room = MEM_W(0, kseg0(kGameQueueCount)) + 4 < MEM_W(0, kseg0(kGameQueueSize));
    while (room && g_in_flight > 0) {
        receive_done(rdram, ctx, hook_ctx, message);
    }
    if (!g_first) {
        if (g_finished.empty()) {
            receive_done(rdram, ctx, hook_ctx, message);  // as the audio thread would: wait for the oldest
        }
        g_previous = g_finished.front();
        g_finished.pop_front();
    }
    g_first = false;
}
