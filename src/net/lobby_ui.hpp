// The ONLINE lobby's page (docs/online-multiplayer-plan.md, M5), drawn by the game itself on the
// title's MULTI barrel; net/net_lobby does the network side.
//
// On the barrel with the skull, Start (Enter on the keyboard) opens it (src/enhancements/debug_menu,
// which says so under the barrel), and so does --net-lobby at boot. Opening it presses A for the
// game, which brings the barrel's face up as it does for the modes; the skull and everything the
// game draws on the face are hidden (hides_sprite) and the lobby's rows are drawn there in the game's
// own font, as the debug menu draws its rows. Closing it presses B, which turns the barrel back to the
// skull. While it is open the game reads no input (input::set_blocked): controller 1's pad steers the
// page (up and down choose, A presses, B goes back) and the keyboard does too (the arrows, Enter,
// Esc). The name and the address or code to join are typed on the keyboard, or picked a letter at a
// time from a keyboard on the barrel with the pad.
//
// Online play is input lockstep, so nothing here touches the game's memory once a session is on:
// in a session the debug menu's key opens the session's page (BACK TO THE GAME, LEAVE THE SESSION),
// which net/net_play draws in its own display list over the picture, above the memory every machine
// keeps the same.
//
// CONKER_LOBBY_SCRIPT=<file> drives the lobby for tests, a line each: "<ms> <action> [text]", the
// time from the page first opening; actions: name <text>, host [port], join <address or code>,
// joinlan [host name], ready, start, kick <n>, leave, leavesession, quit, close, open,
// setenv NAME=value (for the session the lobby starts), shot <file> (writes the View as text) and
// key <key> (a key as if pressed on the page: up, down, left, right, enter, esc, back, or text:<chars>).
// The actions call what the page's rows call.
#pragma once

#include <cstdint>

#include "recomp.h"

union SDL_Event;
struct SDL_Window;

namespace conker::lobby_ui {

// The game's window, once made (main thread): loads the script, and opens the page for --net-lobby.
void set_window(SDL_Window* window);

void open();
void close();
bool is_open();

// Every SDL event, on the main thread; true if the page used it (keys while it is open).
bool handle_event(const SDL_Event& event);

// On the main thread, often: the script, the clipboard and the keyboard's text input.
void update();

// Once a frame on the game thread, from the debug menu's tick (on every screen, before the screen's
// handler reads the frame's presses): steers and draws the page.
void frame(uint8_t* rdram, recomp_context* ctx);

// Whether a menu sprite (func_1510D0EC's resource) is kept from being drawn: the barrel's own while
// the page is on it. Game thread.
bool hides_sprite(uint32_t resource);

}  // namespace conker::lobby_ui
