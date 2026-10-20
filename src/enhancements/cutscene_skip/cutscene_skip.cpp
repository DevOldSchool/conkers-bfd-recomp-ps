// Every cutscene can be skipped with L or Start: ones not watched before, and ones the game never
// lets you skip, such as the new game's opening. Native helpers called from the [[patches.hook]]
// snippets in config/us.toml (func_1501E05C, func_1501BBB8). CONKER_SKIP_CUTSCENES=0 keeps the
// game's rule, where only L skips, only cutscenes already watched, and some not at all.
#include <chrono>
#include <cstdint>
#include <cstdlib>

#include "recomp.h"
#include "enhancements/status.hpp"

namespace {

bool skip_cutscenes_enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("CONKER_SKIP_CUTSCENES");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}

inline gpr kseg0(uint32_t address) { return static_cast<gpr>(static_cast<int32_t>(address)); }

constexpr uint16_t kStartButton = 0x1000;
constexpr uint16_t kLButton = 0x0020;
constexpr uint32_t kScriptClock = 0x800C35B0;  // D_800C35B0[n]: the script's clock
constexpr uint32_t kScriptEnd = 0x800C3640;    // D_800C3640[n]: its length
constexpr uint32_t kScriptChained = 0x800C3C99; // D_800C3C99: lifts the 30-tick rule

// When func_1501E05C last found a skip possible, but for the button.
std::chrono::steady_clock::time_point skippable_at;
constexpr std::chrono::milliseconds kSkippableWindow{100};
bool start_is_l = false;  // a Start press is being passed on as L until it is let go

}  // namespace

// func_1501E05C decides whether the running cutscene script may be skipped this frame. The intro
// (scene 0x21) has a rule of its own, left alone (see intro_skip.cpp). Otherwise it wants L held
// (D_800BE710 & 0x20), D_800C3C9C clear, and then func_1501D2C4(scene, script) to say the script
// has been watched: its bit in the scene's 64-bit mask at D_800C3A60, which func_1501D258 sets as
// a script plays. The remaining conditions stay: more than 30 ticks of the script left (unless
// D_800C3C99), and in func_1501E1B4, which runs the skip, at least 91. The skip (func_1501DF04)
// runs the script's remaining events, so a skipped cutscene leaves the game as a watched one would.

// In the title scene, 0x1D, only script 5 reaches the L check; the others return 0. Scripts 2 to 4
// are part of the new game's opening (Conker telling the story in the bar), and script 1 is the
// title's own opening, which Start already skips. Called at 0x1501E0A4 with the script in $t7,
// compared with 5 next: it becomes 5, so every title scene script goes on to the L check.
extern "C" void conker_skip_title_scripts(uint8_t* /*rdram*/, recomp_context* ctx) {
    if (skip_cutscenes_enabled()) {
        ctx->r15 = 5;
    }
}

// The game skips with L, and Start pauses, also during a cutscene. For Start to skip instead, the
// L test notes when a skip would go ahead with L: at 0x1501E118, before $t3 (buttons & 0x20) is
// tested, with the script's index in the argument slot at $sp + 0x18. It wants at least 91 ticks
// of the script left (func_1501E1B4) and more than 30 unless D_800C3C99 is set (func_1501E05C).
extern "C" void conker_skip_note_window(uint8_t* rdram, recomp_context* ctx) {
    const int32_t index = MEM_W(0x18, ctx->r29);
    const int32_t clock = MEM_W(0, kseg0(kScriptClock + 4 * index));
    const int32_t end = MEM_W(0, kseg0(kScriptEnd + 4 * index));
    if (end - clock >= 91 && (MEM_BU(0, kseg0(kScriptChained)) != 0 || clock + 30 < end)) {
        skippable_at = std::chrono::steady_clock::now();
    }
}

// func_1501BBB8 reads the controllers into $sp + 0x38 (controller 1's buttons first) and copies
// them to D_800BE710 and the rest of the game after 0x1501BC18. While a cutscene can be skipped,
// a Start press becomes L, so it skips the cutscene and the pause menu never sees it. It stays L
// until Start is let go, so the end of the skip does not see a fresh press and pause. When the
// game is paused, or no skip is possible, cutscene or not, Start is left alone.
extern "C" void conker_skip_start_as_l(uint8_t* rdram, recomp_context* ctx) {
    if (!skip_cutscenes_enabled()) {
        return;
    }
    const uint16_t buttons = MEM_HU(0x38, ctx->r29);
    if ((buttons & kStartButton) == 0) {
        start_is_l = false;
        return;
    }
    if (!start_is_l && std::chrono::steady_clock::now() - skippable_at > kSkippableWindow) {
        return;
    }
    start_is_l = true;
    MEM_H(0x38, ctx->r29) = static_cast<int16_t>((buttons & ~kStartButton) | kLButton);
}

// D_800C3C9C makes the running script unskippable, watched or not. func_1501DAAC sets it when a
// script starts with an event 0xE, which every script of the new game's opening has, and
// func_1509C440 can set it during one. Called at 0x1501E128 with it in $t4: it becomes 0.
// Skipped, the opening's scripts still end through their skip handlers (func_1501E81C with code
// 0xF), which go to scene 0x29 where Conker wakes up, from the throne room (0x18 script 1) or the
// bar (0x1D scripts 2 to 4). The scripts passed over are not marked watched, as they never ran.
extern "C" void conker_skip_unskippable(uint8_t* /*rdram*/, recomp_context* ctx) {
    if (skip_cutscenes_enabled()) {
        ctx->r12 = 0;
    }
}

// Called at 0x1501E158, after the call to func_1501D2C4, with its answer in $v0: it becomes 1.
extern "C" void conker_skip_unwatched(uint8_t* /*rdram*/, recomp_context* ctx) {
    if (skip_cutscenes_enabled()) {
        ctx->r2 = 1;
    }
}

nlohmann::json conker::status::cutscene_skip() {
    return enhancement("cutscene_skip", "Skip any cutscene", skip_cutscenes_enabled(), "CONKER_SKIP_CUTSCENES");
}
