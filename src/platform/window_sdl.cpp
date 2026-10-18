// SDL2 window for the RT64 renderer. ultramodern calls create_gfx and create_window on the main
// thread before the game starts and update_gfx from the main loop every millisecond, so this is
// also where SDL events are pumped and controller 1 is sampled (macOS requires all of it on the
// main thread).
//
// The window opens at the largest whole multiple of the game's picture that fits the display, so
// RT64 (which scales by the window height, src/platform/renderer_rt64.cpp) renders at exactly that
// multiple and draws every game pixel as an n x n block, without resampling. CONKER_ORIGINAL_SIZE=1
// keeps the original 960x720 window instead, which is quicker for test runs.
//
// The display mode (src/platform/display_mode.hpp) is full screen unless the OPTIONS menu or
// CONKER_DISPLAY says otherwise: macOS full screen in a Space of its own, a borderless window
// covering the screen in the current Space, or the window above. F11, Cmd+Enter, Alt+Enter or
// Ctrl+Cmd+F switch between the window and the last screen-covering mode. Covering the screen,
// RT64 renders at the multiple that covers the screen height and shrinks it slightly to fit (7x on
// a 1440-line display).
#include "platform/window.hpp"

#include "platform/display_mode.hpp"
#include "platform/input_sdl.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include <SDL.h>
#include <SDL_syswm.h>

#if defined(__APPLE__)
#include <objc/message.h>
#include <objc/runtime.h>
#endif

namespace conker::window {

namespace {

SDL_Window* g_window = nullptr;

// The picture RT64 presents. Conker's VI shows a 292-pixel-wide frame buffer (VI_WIDTH 292,
// VI_X_SCALE 0.5 over H_START 0x8C..0x2D5) and 214 lines (V_START 0x3D..0x1E8), inside black
// borders that RT64 crops (removeBlackBorders). RT64's VI::fbSize rounds the height up to 216.
constexpr int kPictureWidth = conker::display::kPictureWidth;
constexpr int kPictureHeight = conker::display::kPictureHeight;

constexpr int kOriginalWidth = 960;  // the window used before it was sized to the display
constexpr int kOriginalHeight = 720;

// Room for the title bar (28 points on macOS), which is not part of the window size.
constexpr int kTitleBarAllowance = 32;

// The largest multiples of the picture that fit the usable area (without the menu bar and Dock)
// of the primary display, where the centred window opens, and the whole display.
void find_limits() {
    SDL_Rect usable;
    SDL_Rect bounds;
    if (SDL_GetDisplayUsableBounds(0, &usable) != 0 || SDL_GetDisplayBounds(0, &bounds) != 0) {
        std::fprintf(stderr, "[conker] SDL_GetDisplayBounds failed: %s\n", SDL_GetError());
        return;
    }
    const int window_max = std::min(usable.w / kPictureWidth, (usable.h - kTitleBarAllowance) / kPictureHeight);
    // The smallest multiple covering the screen height (RT64 shrinks it to fit), unless the width
    // already limits it.
    const int screen_max = std::min((bounds.w + kPictureWidth - 1) / kPictureWidth,
                                    (bounds.h + kPictureHeight - 1) / kPictureHeight);
    conker::display::set_limits(window_max, screen_max);
    std::printf("[conker] display %dx%d, %dx%d usable: up to %dx in a window, %dx covering the screen\n", bounds.w,
                bounds.h, usable.w, usable.h, window_max, screen_max);
}

bool original_size() {
    const char* value = std::getenv("CONKER_ORIGINAL_SIZE");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}

using conker::display::Mode;

Mode g_screen_mode = Mode::Fullscreen;  // where the shortcuts go from the window

// The window's size when it is a window.
SDL_Rect window_frame() {
    SDL_Rect frame{static_cast<int>(SDL_WINDOWPOS_CENTERED), static_cast<int>(SDL_WINDOWPOS_CENTERED), kOriginalWidth,
                   kOriginalHeight};
    if (!original_size()) {
        frame.w = kPictureWidth * conker::display::window_scale();
        frame.h = kPictureHeight * conker::display::window_scale();
    }
    return frame;
}

// Leaving a full-screen Space is animated, and when it ends macOS puts the window back at the frame
// it had before, after apply_mode has set the new one; coming back from the borderless frame, it is
// also left maximized, which makes SDL ignore new sizes. So for a while after a change the window is
// un-maximized and given the frame the mode wants whenever it has another.
constexpr Uint64 kSettleMs = 3000;
SDL_Rect g_wanted_frame{};  // x and y SDL_WINDOWPOS_CENTERED for the window
Uint64 g_settle_until = 0;

// A borderless window can only cover the menu bar while the app hides it and the Dock
// (NSApplication.presentationOptions); macOS shows them again whenever another app is in front.
void hide_menu_bar(bool hide) {
#if defined(__APPLE__)
    constexpr unsigned long kHideDock = 1ul << 1;
    constexpr unsigned long kHideMenuBar = 1ul << 3;
    id app = reinterpret_cast<id (*)(Class, SEL)>(objc_msgSend)(objc_getClass("NSApplication"),
                                                                 sel_registerName("sharedApplication"));
    reinterpret_cast<void (*)(id, SEL, unsigned long)>(objc_msgSend)(app, sel_registerName("setPresentationOptions:"),
                                                                    hide ? kHideDock | kHideMenuBar : 0);
#else
    (void)hide;
#endif
}

// Every change goes back to the plain window first, so SDL keeps the window's own size to return
// to. Full screen is then SDL's desktop full screen, a Space of its own on macOS. Borderless is a
// window without a frame laid over the whole display in the current Space; SDL's exclusive full
// screen would do that too, but it may change the display mode.
void set_frame(const SDL_Rect& frame) {
    if ((SDL_GetWindowFlags(g_window) & SDL_WINDOW_MAXIMIZED) != 0) {
        SDL_RestoreWindow(g_window);
    }
    SDL_SetWindowSize(g_window, frame.w, frame.h);
    SDL_SetWindowPosition(g_window, frame.x, frame.y);
}

void keep_frame() {
    if (SDL_GetTicks64() >= g_settle_until || (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN) != 0) {
        return;
    }
    int x, y, w, h;
    SDL_GetWindowPosition(g_window, &x, &y);
    SDL_GetWindowSize(g_window, &w, &h);
    const bool centered = g_wanted_frame.x == static_cast<int>(SDL_WINDOWPOS_CENTERED);
    if (w != g_wanted_frame.w || h != g_wanted_frame.h || (!centered && (x != g_wanted_frame.x || y != g_wanted_frame.y))) {
        set_frame(g_wanted_frame);
    }
}

void apply_mode(Mode mode) {
    if (SDL_SetWindowFullscreen(g_window, 0) != 0) {
        std::fprintf(stderr, "[conker] SDL_SetWindowFullscreen failed: %s\n", SDL_GetError());
        return;
    }
    hide_menu_bar(false);
    SDL_SetWindowBordered(g_window, SDL_TRUE);
    g_wanted_frame = window_frame();
    set_frame(g_wanted_frame);
    if (mode == Mode::Fullscreen) {
        if (SDL_SetWindowFullscreen(g_window, SDL_WINDOW_FULLSCREEN_DESKTOP) != 0) {
            std::fprintf(stderr, "[conker] SDL_SetWindowFullscreen failed: %s\n", SDL_GetError());
        }
    } else if (mode == Mode::Borderless) {
        SDL_Rect bounds;
        if (SDL_GetDisplayBounds(std::max(SDL_GetWindowDisplayIndex(g_window), 0), &bounds) == 0) {
            hide_menu_bar(true);
            SDL_SetWindowBordered(g_window, SDL_FALSE);
            g_wanted_frame = bounds;
            set_frame(g_wanted_frame);
        }
    }
    g_settle_until = SDL_GetTicks64() + kSettleMs;
    SDL_ShowCursor(mode == Mode::Windowed ? SDL_ENABLE : SDL_DISABLE);
    if (mode != Mode::Windowed) {
        g_screen_mode = mode;
    }
}

bool is_fullscreen_toggle(const SDL_KeyboardEvent& key) {
    if (key.repeat) {
        return false;
    }
    const SDL_Scancode code = key.keysym.scancode;
    const Uint16 mod = key.keysym.mod;
    return code == SDL_SCANCODE_F11 ||
           ((code == SDL_SCANCODE_RETURN || code == SDL_SCANCODE_KP_ENTER) && (mod & (KMOD_GUI | KMOD_ALT))) ||
           (code == SDL_SCANCODE_F && (mod & KMOD_GUI) && (mod & KMOD_CTRL));
}

} // namespace

void* create_gfx() {
    SDL_SetHint(SDL_HINT_VIDEO_HIGHDPI_DISABLED, "1");
    // SDL 3's SDL_HINT_VIDEO_SYNC_WINDOW_OPERATIONS (through sdl2-compat): leaving full screen
    // finishes before the next window change, so a borderless window is not resized mid-animation.
    SDL_SetHint("SDL_VIDEO_SYNC_WINDOW_OPERATIONS", "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        std::fprintf(stderr, "[conker] SDL_Init failed: %s\n", SDL_GetError());
        return nullptr;
    }
    conker::input::init();
    return &g_window;
}

ultramodern::renderer::WindowHandle create_window(void* /*gfx_data*/) {
    Uint32 flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
#if defined(__APPLE__)
    flags |= SDL_WINDOW_METAL;
#elif defined(__linux__)
    flags |= SDL_WINDOW_VULKAN;
#endif
    find_limits();
    const Mode saved = conker::display::startup_mode();
    const Mode mode = original_size() ? Mode::Windowed : saved;
    const SDL_Rect frame = window_frame();
    if (original_size()) {
        std::printf("[conker] CONKER_ORIGINAL_SIZE: window %dx%d\n", frame.w, frame.h);
    }
    g_window = SDL_CreateWindow("Conker's Bad Fur Day: Recompiled", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                frame.w, frame.h, flags);
    if (g_window == nullptr) {
        std::fprintf(stderr, "[conker] SDL_CreateWindow failed: %s\n", SDL_GetError());
        return {};
    }
    conker::display::started(mode);
    std::printf("[conker] display mode: %s\n", conker::display::name(mode));
    if (mode != Mode::Windowed) {
        apply_mode(mode);
    }

    SDL_SysWMinfo wm_info;
    SDL_VERSION(&wm_info.version);
    if (!SDL_GetWindowWMInfo(g_window, &wm_info)) {
        std::fprintf(stderr, "[conker] SDL_GetWindowWMInfo failed: %s\n", SDL_GetError());
        return {};
    }

#if defined(_WIN32)
    return ultramodern::renderer::WindowHandle{wm_info.info.win.window, GetCurrentThreadId()};
#elif defined(__APPLE__)
    // plume's Metal backend draws into the CAMetalLayer of an SDL Metal view.
    SDL_MetalView view = SDL_Metal_CreateView(g_window);
    return ultramodern::renderer::WindowHandle{wm_info.info.cocoa.window, SDL_Metal_GetLayer(view)};
#else
    return g_window;
#endif
}

void update_gfx(void* /*gfx_data*/) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) {
            ultramodern::quit();
        }
        if (event.type == SDL_KEYDOWN && is_fullscreen_toggle(event.key)) {
            conker::display::request(conker::display::current() == Mode::Windowed ? g_screen_mode : Mode::Windowed);
        }
        conker::input::handle_event(event);
    }
    if (Mode mode; conker::display::take_request(&mode)) {
        apply_mode(mode);
    }
    keep_frame();
    conker::input::update();
}

} // namespace conker::window
