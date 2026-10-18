// The display mode and resolution: a window, a borderless window covering the screen, or full
// screen, and how large the game's 292x216 picture is drawn in each. They are chosen in the
// OPTIONS menu (src/enhancements/display_option/display_option.cpp) or, for the mode, with the
// full-screen shortcuts, kept in conker-recomp-data/display.cfg, and applied by the SDL window
// (src/platform/window_sdl.cpp) and RT64 (src/platform/renderer_rt64.cpp). The null build keeps
// the settings but has no window to apply them to.
#pragma once

namespace conker::display {

// The values are the OPTIONS menu row's values, left to right.
enum class Mode : int { Windowed = 0, Borderless = 1, Fullscreen = 2 };
constexpr int kModeCount = 3;

// The picture RT64 presents: Conker's frame buffer with the VI's black borders cropped (see
// src/platform/window_sdl.cpp).
constexpr int kPictureWidth = 292;
constexpr int kPictureHeight = 216;

// The smallest multiple of the picture offered in the menu.
constexpr int kMinScale = 1;

const char* name(Mode mode);

// Reads conker-recomp-data/display.cfg. The mode to start in is CONKER_DISPLAY (windowed,
// borderless or fullscreen) if set, else the saved one, else full screen.
Mode startup_mode();

// Records the mode the window started in, without saving it.
void started(Mode mode);

// The mode in use, or requested and not yet applied. Safe to call from any thread.
Mode current();

// Asks for another mode and saves it. Safe to call from any thread; the window applies it on the
// main thread (take_request).
void request(Mode mode);

// The largest multiples of the picture that fit a window on the display (title bar and Dock
// allowed for) and the whole screen; set by the window when it opens.
void set_limits(int window_max, int screen_max);
int window_max();
int screen_max();

// The window's multiple of the picture (kMinScale..window_max()), and the screen's: 0 fills the
// screen (RT64 renders at the multiple covering its height and shrinks it to fit), n draws it at
// exactly n times, centred.
int window_scale();
int screen_scale();

// Ask for another multiple and save it; safe to call from any thread.
void request_window_scale(int scale);
void request_screen_scale(int scale);

// For the window: whether the mode or the window's multiple changed since the last call, and the
// mode to be in.
bool take_request(Mode* mode);

} // namespace conker::display
