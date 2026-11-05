// The debug menu's CONTROLS page, opened on its own from the OPTIONS page's CONTROLS row
// (src/enhancements/display_options): it takes the OPTIONS page's place, the world going on behind
// it (no pause), and B goes back to OPTIONS. Not online, where only this machine's game would stop
// reading the pad and drawing OPTIONS, setting it apart from the others.
#pragma once

#include <cstdint>
#include <string>

#include "recomp.h"

namespace conker::debug_menu {

bool controls_available();

// Opens the page at the next frame, on a menu screen; game thread.
void open_controls();

// The game's own HUD text, as this menu draws it (func_15042D94 with a shadow, the 292x216 picture's
// pixels, x the left end or the middle): for the ONLINE lobby (src/net/lobby_ui.cpp). Game thread,
// inside conker_debug_menu_tick. Colours are RGBA.
constexpr int kPictureCenterX = 0x92;
void draw_text(uint8_t* rdram, recomp_context* ctx, int x, int y, bool centered, uint32_t color, const std::string& text,
               float scale);
// The pause page's lever sound (moving between rows) or its select sound.
void play_menu_sound(uint8_t* rdram, recomp_context* ctx, bool select);

// SAVE DEBUG REPORT from the online session page (src/net/lobby_ui.cpp), where the debug
// menu is off: the report is made on the game thread at its next frame. A match has no save, so it
// is report.json only, with every player's position and camera and which one is this machine's;
// nothing in the game's memory is written, so the session stays in step. Any thread.
void request_report();

// What the last request_report made: "" before one, else a line to show (the report's folder, or why
// there is none). Any thread.
std::string report_result();

}  // namespace conker::debug_menu
