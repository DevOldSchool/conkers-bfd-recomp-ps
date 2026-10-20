// Every cutscene can be skipped with L: ones not watched before, and ones the game never lets you
// skip, such as the new game's opening. Native helpers called from the [[patches.hook]] snippets
// in config/us.toml (func_1501E05C). CONKER_SKIP_CUTSCENES=0 keeps the game's rule, where only
// cutscenes already watched can be skipped, and some not at all.
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
// title's own opening, which the intro skip already passes over (intro_skip.cpp). Called at
// 0x1501E0A4 with the script in $t7, compared with 5 next: it becomes 5, so every title scene
// script goes on to the L check.
extern "C" void conker_skip_title_scripts(uint8_t* /*rdram*/, recomp_context* ctx) {
    if (skip_cutscenes_enabled()) {
        ctx->r15 = 5;
    }
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
