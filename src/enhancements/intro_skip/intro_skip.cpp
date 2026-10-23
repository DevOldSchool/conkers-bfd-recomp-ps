// L skips from the boot logos or the intro straight to the title menu, as it skips cutscenes
// (src/enhancements/cutscene_skip). Native helpers called from the [[patches.hook]] snippets in
// config/us.toml (func_151DE6D4, func_1501E05C, func_1501E1B4, func_1517F3A0).
// CONKER_SKIP_INTRO=1 skips without a press, =0 turns it off.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>

#include "recomp.h"
#include "enhancements/pause_save/resume_point.hpp"
#include "enhancements/status.hpp"
#include "enhancements/switches.hpp"

namespace {

inline gpr kseg0(uint32_t address) { return static_cast<gpr>(static_cast<int32_t>(address)); }

constexpr uint32_t kButtons = 0x800BE710;        // D_800BE710: controller 1's buttons
constexpr uint16_t kLButton = 0x0020;
constexpr uint32_t kLogoTimer = 0x800E0A90;      // D_800E0A90: boot logo ticks
constexpr int32_t kLogoEnd = 0x349;              // func_151DE6D4 loads the intro from here
constexpr uint32_t kTitleSkipFlag = 0x8008FD84;  // D_8008FD84: skip the title scene's script

// Unset: L skips to the title menu. CONKER_SKIP_INTRO=1 skips without a press.
// CONKER_SKIP_INTRO=0 keeps the hardware rule (Start, only after a warm reset), so the intro
// always plays, as the runtime cold boots.
enum class IntroSkip { Never, OnPress, Always };

IntroSkip variable_mode() {
    const char* value = std::getenv("CONKER_SKIP_INTRO");
    if (value == nullptr || value[0] == '\0') {
        return IntroSkip::OnPress;
    }
    return value[0] == '0' ? IntroSkip::Never : IntroSkip::Always;
}

// The debug menu switches it between Never and the variable's mode (OnPress if that is Never).
std::atomic<IntroSkip> g_mode{variable_mode()};

IntroSkip intro_skip_mode() { return g_mode.load(std::memory_order_relaxed); }

// One skip per boot. An L press during the logos or the intro fades the screen to black, the
// skips then run behind it, and it fades back in on the title menu. Only the game thread touches
// this.
enum class Skip {
    Idle,       // no press yet
    FadingOut,  // L pressed, the screen is fading to black
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

// Notes an L press (if `read_press`) and moves the fade along. True once the screen is black.
bool skip_to_title_menu(uint8_t* rdram, bool read_press) {
    if (intro_skip_mode() == IntroSkip::Never) {
        return false;
    }
    if (skip == Skip::Idle && read_press &&
        (intro_skip_mode() == IntroSkip::Always || (MEM_HU(0, kseg0(kButtons)) & kLButton) != 0)) {
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
// black, so the scenes it passes through stay hidden; so too while a save the debug menu started
// loads (conker::resume::load_cover). Only the drawing changes; game logic that asks func_1517EF00
// about the fade sees the game's own value.
extern "C" void conker_skip_fade_alpha(uint8_t* rdram, recomp_context* ctx) {
    const int32_t alpha = std::max(skip_fade(rdram), conker::resume::load_cover());
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

nlohmann::json conker::status::intro_skip() {
    const IntroSkip mode = intro_skip_mode();
    // "press": a press of L skips; "always": skipped without a press; "never": the original behaviour.
    const char* name = mode == IntroSkip::Always ? "always" : mode == IntroSkip::Never ? "never" : "press";
    return enhancement("intro_skip", "Skip to the title menu", mode != IntroSkip::Never, "CONKER_SKIP_INTRO",
                       {{"mode", name}});
}

// Switched off part way through, a skip stops where it is and the screen is shown again.
conker::switches::Switch conker::switches::intro_skip() {
    return {"SKIP INTRO", [] { return intro_skip_mode() != IntroSkip::Never; },
            [](bool on) {
                const IntroSkip mode = variable_mode();
                g_mode.store(!on ? IntroSkip::Never : mode == IntroSkip::Never ? IntroSkip::OnPress : mode);
                if (!on && skip != Skip::Idle) {
                    skip = Skip::Done;
                }
            }};
}
