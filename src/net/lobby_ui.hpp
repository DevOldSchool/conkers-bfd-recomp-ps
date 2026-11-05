// The ONLINE lobby window (docs/online-multiplayer-plan.md, M5): a Dear ImGui window over the game,
// using the ImGui that RT64 ships. Opened with Start on the title's MULTI door (src/enhancements/
// debug_menu/debug_menu.cpp, which says so under the door) or with --net-lobby;
// net/net_lobby does the network side.
//
// The window's frame is built on the main thread, where SDL's events come in (update()), and drawn
// into the swap chain by RT64's present thread through its draw hook. While it is open the game reads
// no input (input::set_blocked) and the window takes the keyboard, the mouse and controller 1, whose
// d-pad or stick moves between the buttons, A presses one and B goes back.
//
// CONKER_LOBBY_SCRIPT=<file> drives the lobby for tests, a line each: "<ms> <action> [text]", the
// time from the window first opening; actions: name <text>, host [port], join <address or code>,
// ready, start, leave, close, open, report (SAVE DEBUG REPORT in a session), shot <file> (writes
// the View as text), setenv NAME=value (for the session the lobby starts). The actions call what the
// window's buttons call.
#pragma once

union SDL_Event;
struct SDL_Window;

namespace RT64 {
struct Application;
}

namespace conker::lobby_ui {

// The game's window, once made (main thread), and, once RT64 is set up (on its thread), the
// application whose device and swap chain the lobby's window draws with.
void set_window(SDL_Window* window);
void attach(RT64::Application* app);

// The window (the null renderer's build has none: these do nothing there).
void open();
void close();
bool is_open();

// Every SDL event, on the main thread; true if the window used it (the keyboard and mouse while open).
bool handle_event(const SDL_Event& event);

// On the main thread, often: builds the window's next frame when one is due.
void update();

}  // namespace conker::lobby_ui
