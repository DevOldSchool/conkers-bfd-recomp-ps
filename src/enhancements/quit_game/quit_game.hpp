// EXIT GAME on the title menu: closing the game from inside it, for a player with only a pad (or in
// full screen, where the window's close button is out of reach). While the title turns between its
// GAME 1, MULTI, OPTIONS and CHAPTERS corners, Start (which otherwise opens the corner, as A does)
// opens a menu: RESUME and EXIT GAME. A on EXIT GAME asks EXIT GAME? with NO and YES (NO first),
// and A on YES closes the game as the window's close button does; B or Start goes back. Run from
// the debug menu's frame tick (src/enhancements/debug_menu), which every screen has.
// CONKER_QUIT_GAME=0 leaves it out.
#pragma once

#include <cstdint>

#include "recomp.h"

namespace conker::quit_game {

// Whether EXIT GAME is offered: the switch is on, and not online, where the session's own window
// (the debug menu's key) has LEAVE THE SESSION and every machine must read the pads alike.
bool offered();

// Once a frame on every screen, from the debug menu's tick while that menu and the online lobby are
// closed: opens, draws and steers the title's Start menu with controller 1. Game thread.
void frame(uint8_t* rdram, recomp_context* ctx);

// True while the menu is open, and after until controller 1's buttons are let go: the game is
// then given empty pads, so the title does not turn or open a corner underneath. Game thread.
bool holds_pads();

// Closes the game as the window's close button does: the runtime stops, main returns and every
// exit handler runs (an online session is left, the crash log sees a normal quit). Any thread.
void quit();

}  // namespace conker::quit_game
