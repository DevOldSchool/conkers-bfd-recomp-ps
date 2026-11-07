// Keeps a skipped cutscene from leaving the music it was meant to replace playing. Native helpers
// called from the [[patches.hook]] snippets for func_8000D96C and func_8000DE1C in config/us.toml.
#include <cstdint>
#include <cstdio>

#include "recomp.h"

extern "C" void func_8000DE1C(uint8_t* rdram, recomp_context* ctx);

namespace {

inline gpr kseg0(uint32_t address) { return static_cast<gpr>(static_cast<int32_t>(address)); }

constexpr uint32_t kMusicPlayers = 0x800417B0;  // D_800417B0: SequenceRecordState* per player
constexpr int kRecordId = 0x4;                  // SequenceRecordState.id
constexpr int kPlayers = 3;

// The last "play `incoming` in place of `outgoing`" request (func_8000D96C with both ids).
int32_t g_incoming = 0;
int32_t g_outgoing = 0;

bool playing(uint8_t* rdram, int32_t id) {
    for (int p = 0; p < kPlayers; p++) {
        const uint32_t record = static_cast<uint32_t>(MEM_W(4 * p, kseg0(kMusicPlayers)));
        if (record >= 0x80000000u && record < 0x80800000u && MEM_W(kRecordId, kseg0(record)) == id) {
            return true;
        }
    }
    return false;
}

}  // namespace

// func_8000D96C(incoming, outgoing, mode) changes the music. With both ids it queues the incoming
// sequence to take over the outgoing one's player a few frames later, at the outgoing one's next
// sync point. Remember the pair.
extern "C" void conker_music_change_hook(uint8_t* /*rdram*/, recomp_context* ctx) {
    g_incoming = static_cast<int32_t>(ctx->r4) & 0xFFF;
    g_outgoing = static_cast<int32_t>(ctx->r5) & 0xFFF;
}

// func_8000DE1C(id, mode) stops a sequence. Cutscenes swap the level music for their own with
// func_8000D96C when they start and stop their own sequence when they end. Mrs Bee's
// hive cutscene queues sequence 5 in place of the fight's sequence 4 and stops 5 at its end; skipped
// in its first few frames, the stop comes before 5 has taken over, so 5 never plays and 4 is left
// playing over the level for good. When the sequence being stopped is still only queued to
// replace one that is playing, stop that one as well, which is where the watched cutscene ends up.
extern "C" void conker_music_stop_hook(uint8_t* rdram, recomp_context* ctx) {
    const int32_t id = static_cast<int32_t>(ctx->r4) & 0xFFF;
    if (id == 0 || id != g_incoming || g_outgoing == 0 || g_outgoing == id) {
        return;
    }
    const int32_t outgoing = g_outgoing;
    g_incoming = g_outgoing = 0;
    if (playing(rdram, id) || !playing(rdram, outgoing)) {
        return;
    }
    std::printf("[conker] music: stopping sequence %d, which queued sequence %d was to replace\n", outgoing, id);
    std::fflush(stdout);
    const recomp_context saved = *ctx;
    ctx->r4 = outgoing;
    func_8000DE1C(rdram, ctx);
    *ctx = saved;
}
