// Native helpers called from the [[patches.hook]] snippets in config/us.toml.
#include <cstdint>

#include "recomp.h"

namespace {

inline gpr kseg0(uint32_t address) { return static_cast<gpr>(static_cast<int32_t>(address)); }

constexpr uint32_t kSequencePlayers = 0x8003C900;  // D_8003C900: the three ALCSPlayer pointers
constexpr int kPlayerEventAllocList = 0x48 + 0x8;  // ALCSPlayer.evtq.allocList.next
constexpr int kEventType = 0xC;                    // ALEventListItem.evt.type
constexpr int16_t kSeqpPlayEvent = 0xF;            // posted by n_alCSPPlay
constexpr int32_t kStopped = 0;
constexpr int32_t kPlaying = 1;

}  // namespace

// func_8000853C returns a sequence player's state to the music manager (func_8000D2F8,
// func_8000DEC4), which takes 0 (stopped) to mean the sequence has ended and frees its record.
// n_alCSPPlay only queues an event; the audio thread applies it in its next render, every second
// retrace. On hardware that render always comes before the manager's next tick: the game frame
// that starts a sequence takes the N64 CPU long enough, and the audio thread outranks the game
// thread. Recompiled, the frame finishes in about a millisecond, and after a long loading frame
// the game's catch-up pacing starts the next frame one retrace later, before the render. The
// manager then sees the new sequence as stopped and drops it, so the title scene's music
// (sequence 0034) never fades in. Report a stopped player with a PLAY event still queued as
// playing, which is the state hardware would show at that point.
extern "C" void conker_seqp_state_hook(uint8_t* rdram, recomp_context* ctx) {
    if (static_cast<int32_t>(ctx->r2) != kStopped) {
        return;
    }
    const uint32_t index = static_cast<uint32_t>(MEM_W(0x18, ctx->r29)) & 0xFF;
    const gpr player = kseg0(static_cast<uint32_t>(MEM_W(4 * index, kseg0(kSequencePlayers))));
    uint32_t item = static_cast<uint32_t>(MEM_W(kPlayerEventAllocList, player));
    for (int guard = 0; item != 0 && guard < 256; guard++, item = static_cast<uint32_t>(MEM_W(0, kseg0(item)))) {
        if (MEM_H(kEventType, kseg0(item)) == kSeqpPlayEvent) {
            ctx->r2 = kPlaying;
            return;
        }
    }
}
