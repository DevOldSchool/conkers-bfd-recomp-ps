// The display mode: a window, a borderless window covering the screen, or full screen. It is
// chosen in the OPTIONS menu (src/enhancements/display_option/display_option.cpp) or with the
// full-screen shortcuts, kept in conker-recomp-data/display.cfg, and applied by the SDL window
// (src/platform/window_sdl.cpp). The null build keeps the setting but has no window to apply it to.
#pragma once

namespace conker::display {

// The values are the OPTIONS menu row's values, left to right.
enum class Mode : int { Windowed = 0, Borderless = 1, Fullscreen = 2 };
constexpr int kModeCount = 3;

const char* name(Mode mode);

// The mode to start in: CONKER_DISPLAY (windowed, borderless or fullscreen) if set, else the
// saved one, else full screen.
Mode startup_mode();

// Records the mode the window started in, without saving it.
void started(Mode mode);

// The mode in use, or requested and not yet applied. Safe to call from any thread.
Mode current();

// Asks for another mode and saves it. Safe to call from any thread; the window applies it on the
// main thread (take_request).
void request(Mode mode);

// For the window: the mode asked for since the last call, if any.
bool take_request(Mode* mode);

} // namespace conker::display
