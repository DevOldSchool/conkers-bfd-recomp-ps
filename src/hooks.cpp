// Native helpers called from the [[patches.hook]] snippets in config/us.toml.
#include <cstdint>
#include <cstdlib>

#include "recomp.h"
#include "region_escape.h"

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

// --- skipping the intro --------------------------------------------------------------------------

namespace {

constexpr uint16_t kStartButton = 0x1000;

// Unset: Start skips the intro. CONKER_SKIP_INTRO=1 skips it without a press. CONKER_SKIP_INTRO=0
// keeps the hardware rule (only after a warm reset), so it always plays, as the runtime cold boots.
enum class IntroSkip { Never, OnStart, Always };

IntroSkip intro_skip_mode() {
    static const IntroSkip mode = [] {
        const char* value = std::getenv("CONKER_SKIP_INTRO");
        if (value == nullptr || value[0] == '\0') {
            return IntroSkip::OnStart;
        }
        return value[0] == '0' ? IntroSkip::Never : IntroSkip::Always;
    }();
    return mode;
}

}  // namespace

// func_1501E05C decides whether the running cutscene may be skipped. For the intro, scene 0x21,
// it wants Start held (D_800BE710 & 0x1000), osResetType == 1 and the script clock at 301 ticks or
// more. These hooks run inside the scene 0x21 branch: $t8 holds the buttons, $t0 osResetType.
extern "C" void conker_intro_skip_buttons(uint8_t* /*rdram*/, recomp_context* ctx) {
    if (intro_skip_mode() == IntroSkip::Always) {
        ctx->r24 |= kStartButton;
    }
}

extern "C" void conker_intro_skip_reset_type(uint8_t* /*rdram*/, recomp_context* ctx) {
    if (intro_skip_mode() != IntroSkip::Never) {
        ctx->r8 = 1;  // warm reset (NMI)
    }
}

// --- escapes out of the geometry code (see src/region_escape.h) ---------------------------------

namespace {

constexpr uint32_t kGeometryEscapeSp = 0x800D3858;  // D_800D3858: func_150ADAF0's $sp, 0 when idle

struct GeometryEscape {
    jmp_buf buffer;
    uint32_t sp = 0;  // the frame the buffer returns to; 0 when not armed
};

thread_local GeometryEscape t_geometry_escape;

}  // namespace

extern "C" jmp_buf* conker_geometry_escape_arm(recomp_context* ctx) {
    t_geometry_escape.sp = static_cast<uint32_t>(ctx->r29);
    return &t_geometry_escape.buffer;
}

extern "C" void conker_geometry_escape_disarm(uint8_t* /*rdram*/, recomp_context* ctx) {
    if (t_geometry_escape.sp == static_cast<uint32_t>(ctx->r29)) {
        t_geometry_escape.sp = 0;
    }
}

extern "C" void conker_geometry_escape(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t sp = static_cast<uint32_t>(MEM_W(0, kseg0(kGeometryEscapeSp)));
    if (sp == 0 || sp != t_geometry_escape.sp) {
        return;
    }
    // lui $t2; lw $t2, D_800D3858; sw $zero, D_800D3858; b 0x150AE0AC; or $sp, $t2, $zero
    ctx->r10 = kseg0(sp);
    MEM_W(0, kseg0(kGeometryEscapeSp)) = 0;
    ctx->r29 = kseg0(sp);
#if defined(_WIN32)
    longjmp(t_geometry_escape.buffer, 1);
#else
    _longjmp(t_geometry_escape.buffer, 1);
#endif
}
