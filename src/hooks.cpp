// Native helpers called from the [[patches.hook]] snippets in config/us.toml.
#include <algorithm>
#include <chrono>
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

constexpr uint32_t kButtons = 0x800BE710;        // D_800BE710: controller 1's buttons
constexpr uint16_t kStartButton = 0x1000;
constexpr uint32_t kLogoTimer = 0x800E0A90;      // D_800E0A90: boot logo ticks
constexpr int32_t kLogoEnd = 0x349;              // func_151DE6D4 loads the intro from here
constexpr uint32_t kTitleSkipFlag = 0x8008FD84;  // D_8008FD84: skip the title scene's script

// Unset: Start skips to the title menu. CONKER_SKIP_INTRO=1 skips without a press.
// CONKER_SKIP_INTRO=0 keeps the hardware rule (only after a warm reset), so the intro always
// plays, as the runtime cold boots.
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

// One skip per boot. A Start press during the logos or the intro fades the screen to black, the
// skips then run behind it, and it fades back in on the title menu. Only the game thread touches
// this.
enum class Skip {
    Idle,       // no press yet
    FadingOut,  // Start pressed, the screen is fading to black
    Skipping,   // black: the logos and the intro end at their next check
    Revealing,  // the title scene's opening is ending, still black
    FadingIn,   // at the menu, fading back in
    Done,       // finished, or never asked for
};
Skip skip = Skip::Idle;
std::chrono::steady_clock::time_point fade_start;
constexpr std::chrono::milliseconds kFade{330};  // each way

constexpr uint32_t kScene = 0x800BE9F0;       // D_800BE9F0: current scene
constexpr uint32_t kScriptState = 0x800C35EA; // D_800C35EA[0]: 1 while a cutscene script runs
constexpr uint32_t kScriptSkip = 0x800C3C88;  // D_800C3C88: func_1501E1B4's skip fade, 0 when idle
constexpr int32_t kTitleScene = 0x1D;

// Notes a Start press (if `read_start`) and moves the fade along. True once the screen is black.
bool skip_to_title_menu(uint8_t* rdram, bool read_start) {
    if (intro_skip_mode() == IntroSkip::Never) {
        return false;
    }
    if (skip == Skip::Idle && read_start &&
        (intro_skip_mode() == IntroSkip::Always || (MEM_HU(0, kseg0(kButtons)) & kStartButton) != 0)) {
        skip = Skip::FadingOut;
        fade_start = std::chrono::steady_clock::now();
    }
    if (skip == Skip::FadingOut && std::chrono::steady_clock::now() - fade_start >= kFade) {
        skip = Skip::Skipping;
    }
    return skip == Skip::Skipping;
}

// How black the skip wants the screen, 0 (clear) to 0xFF.
int32_t skip_fade(uint8_t* rdram) {
    const auto elapsed = std::chrono::steady_clock::now() - fade_start;
    const int32_t faded = static_cast<int32_t>(std::min<int64_t>(0xFF, 0xFF * elapsed / kFade));
    switch (skip) {
        case Skip::FadingOut:
            return faded;
        case Skip::Skipping:
            return 0xFF;
        case Skip::Revealing:
            // The title scene's script has ended: the menu is up. The game cuts to it, so fade in.
            if (MEM_W(0, kseg0(kScene)) == kTitleScene && MEM_BU(0, kseg0(kScriptState)) == 0 &&
                MEM_BU(0, kseg0(kScriptSkip)) == 0) {
                skip = Skip::FadingIn;
                fade_start = std::chrono::steady_clock::now();
            }
            return 0xFF;
        case Skip::FadingIn:
            if (faded == 0xFF) {
                skip = Skip::Done;
            }
            return 0xFF - faded;
        default:
            return 0;
    }
}

bool fade_forced = false;  // set by conker_skip_fade_alpha for conker_skip_fade_colour

}  // namespace

// func_151DE6D4 runs the boot logos (scene 0x25) on a tick counter and loads the
// intro when it reaches 0x349. A skips ahead to fixed points from 0x14B on; Start goes to the end.
// Not on its first call (0 ticks): loading the intro in the logo scene's first frame hangs the game.
extern "C" void conker_logo_skip(uint8_t* rdram, recomp_context* /*ctx*/) {
    const int32_t ticks = MEM_W(0, kseg0(kLogoTimer));
    if (skip_to_title_menu(rdram, true) && ticks > 0 && ticks < kLogoEnd) {
        MEM_W(0, kseg0(kLogoTimer)) = kLogoEnd;
    }
}

// func_1501E05C decides whether the running cutscene may be skipped. For the intro, scene 0x21,
// it wants Start held, osResetType == 1 (a warm reset, which the runtime never does) and the
// script clock at 301 ticks or more. Called at the start of that branch; the hook returns 1 when
// this sets conker_intro_skip_now.
extern "C" int conker_intro_skip_now = 0;

extern "C" void conker_intro_skip(uint8_t* rdram, recomp_context* /*ctx*/) {
    conker_intro_skip_now = skip_to_title_menu(rdram, true) ? 1 : 0;
}

// The title scene's opening (scene 0x1D script 1, the clock and the Cock and Plucker) is skipped
// when D_8008FD84 is set, which func_151DE8F0 does for any press. Set it for a press made earlier,
// so one press goes all the way to the menu. Called in func_1501E05C's scene 0x1D branch.
extern "C" void conker_title_skip(uint8_t* rdram, recomp_context* /*ctx*/) {
    if (skip_to_title_menu(rdram, false)) {
        MEM_B(0, kseg0(kTitleSkipFlag)) = 1;
        skip = Skip::Revealing;
    }
}

// func_1501E1B4 runs a cutscene skip: it fades out for 30 ticks (D_800C3C89, from $t2), then ends
// the script and fades back in. Behind the skip's black screen that wait only adds a delay, so
// make it one tick. Called before the store.
extern "C" void conker_skip_fade_wait(uint8_t* /*rdram*/, recomp_context* ctx) {
    if (skip == Skip::Skipping || skip == Skip::Revealing) {
        ctx->r10 = 1;
    }
}

// func_1517F3A0 draws a viewport's fade: a full-screen rectangle in the fade colour, with $v0 the
// opacity from func_1517EF00. While the skip runs it is drawn at least as dark as skip_fade, in
// black, so the scenes it passes through stay hidden. Only the drawing changes; game logic that
// asks func_1517EF00 about the fade sees the game's own value.
extern "C" void conker_skip_fade_alpha(uint8_t* rdram, recomp_context* ctx) {
    const int32_t alpha = skip_fade(rdram);
    fade_forced = alpha > static_cast<int32_t>(ctx->r2);
    if (fade_forced) {
        ctx->r2 = alpha;
    }
}

// Called before func_1517F3A0 passes the colour to func_1517F08C ($a2 red, $a3 green, $t8 blue).
extern "C" void conker_skip_fade_colour(uint8_t* /*rdram*/, recomp_context* ctx) {
    if (fade_forced) {
        ctx->r6 = 0;
        ctx->r7 = 0;
        ctx->r24 = 0;
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
