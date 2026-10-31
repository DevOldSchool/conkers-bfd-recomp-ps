// The display mode and resolution: a window, a borderless window covering the screen, or full
// screen, how large the game's 292x216 picture is drawn in each, and whether it is widened to the
// window's or screen's shape, and whether the sharper text and menus are drawn. They are chosen in
// the OPTIONS menu (src/enhancements/display_options/display_options.cpp) or, for the mode, with the
// full-screen shortcuts, kept in conker-recomp-data/display.cfg, and applied by the SDL window
// (src/platform/window_sdl.cpp) and RT64 (src/platform/renderer_rt64.cpp). The null build keeps
// the settings but has no window to apply them to.
#pragma once

#include <cstdint>

namespace conker::display {

// The values are the OPTIONS menu row's values, left to right.
enum class Mode : int { Windowed = 0, Borderless = 1, Fullscreen = 2 };
constexpr int kModeCount = 3;

// The picture RT64 presents: Conker's frame buffer with the VI's black borders cropped (see
// src/platform/window_sdl.cpp).
constexpr int kPictureWidth = 292;
constexpr int kPictureHeight = 216;

// The window's picture in widescreen: 16:9 at the same height, which RT64 widens the game's view
// to fill.
constexpr int kWidePictureWidth = 384;

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

// Whether the window is still changing to the mode last asked for: from request() until the window
// has settled in it. RT64 keeps the multiple, fill and shape it renders with until then. Leaving or
// entering a macOS full-screen Space while RT64 also discarded and remade its frame buffers at
// another size stalled the main thread, and the game with it, for up to half a minute.
bool window_changing();

// For the window: the number of mode requests so far, read when it takes one, and given back once
// the window has settled in the mode, which ends window_changing unless another came since.
uint32_t mode_requests();
void set_window_settled(uint32_t requests);

// The largest multiple of the picture that fits a window on the display (title bar and Dock
// allowed for), and the smallest that covers the whole screen, in 4:3 and in widescreen, with the
// screen's size; set by the window when it opens.
struct Limits {
    int window_max, wide_window_max;
    int screen_max, wide_screen_max;
    int screen_width, screen_height;
};
void set_limits(const Limits& limits);

// The limits for the picture's current shape.
int window_max();

// The display's size in pixels, 0 until the window has measured it.
int display_width();
int display_height();
int screen_max();

// Whether the picture is widened from Conker's 4:3 to the shape of the window or screen (RT64's
// Expand aspect ratio): 16:9 for a window opened at its multiple, the display's own shape covering
// the screen. Like the fill, a window and the screen-covering modes each keep their own: covering
// the screen is widescreen by default, a window is 4:3 as on an N64.
bool widescreen(Mode mode);

// The width of the picture at a multiple in a window (kPictureWidth or kWidePictureWidth times the
// multiple) or covering the screen (widened to the screen's shape in widescreen).
int window_picture_width(int scale);
int screen_picture_width(int scale);

// The window's multiple of the picture (kMinScale..window_max(), the largest by default), and the
// one rendered when covering the screen (kMinScale..screen_max(), the largest by default).
int window_scale();
int screen_scale();

// Whether the picture is scaled to fill the window or screen, or drawn at its own size, centered.
// Each mode remembers its own: a window is centered by default (it is the picture's size unless it
// is resized by hand), covering the screen fills it by default.
bool fill(Mode mode);

// Ask for another setting and save it; safe to call from any thread. Asking for the largest
// multiple saves "the largest", which follows the display's limits.
void request_window_scale(int scale);
void request_screen_scale(int scale);
void request_fill(Mode mode, bool fill);
void request_widescreen(Mode mode, bool widescreen);

// The pause menu's background is a copy of the last frame that RT64 keeps at its rendered size and
// shape, made once when the menu opens (src/enhancements/display_options/widescreen_copies.cpp). A
// new resolution or shape would rebuild RT64's frame buffers and lose it, so while such copies are
// being drawn the renderer holds its settings and applies new ones afterwards. note_frame_copy is
// called for each copy the game draws; frame_copy_count is how many there have been.
void note_frame_copy();
uint32_t frame_copy_count();

// Whether RT64 is rendering a widened picture right now (set by the renderer), which can lag the
// setting while the renderer holds it.
void set_rendered_widescreen(bool widescreen);
bool rendered_widescreen();

// Whether the built-in sharper text and menus are drawn (src/enhancements/sharp_font,
// src/enhancements/sharp_menus), chosen in the OPTIONS menu; on by default. The renderer loads or
// drops them when it changes.
bool sharp();
void request_sharp(bool sharp);

// From now on, this run's widescreen and sharp settings are not saved: display.cfg keeps the
// player's own as they are now. Online, a guest plays with the host's (net_play.hpp), for that
// session only.
void keep_saved_picture();

// The shape (width / height) of the picture RT64 is rendering, set by the renderer every screen
// refresh: the picture's own 292:216 in 4:3, the window's or screen's shape widened.
void set_rendered_aspect(float aspect);
float rendered_aspect();

// The shape the game's own code works to (widescreen culling, the widened frame copies): the
// rendered one, or online one fixed for the session (set_logic_aspect), since the window's shape
// differs between machines and changes at a moment that is not a game frame.
void set_logic_aspect(float aspect);
float logic_aspect();

// For the window: whether the mode, the window's multiple or the picture's shape changed since the
// last call, and the mode to be in.
bool take_request(Mode* mode);

} // namespace conker::display
