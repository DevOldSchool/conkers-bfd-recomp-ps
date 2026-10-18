// SDL2 window for the RT64 renderer. ultramodern calls create_gfx and create_window on the main
// thread before the game starts and update_gfx from the main loop every millisecond, so this is
// also where SDL events are pumped and controller 1 is sampled (macOS requires all of it on the
// main thread).
//
// The window opens at the largest whole multiple of the game's picture that fits the display, so
// RT64 (which scales by the window height, src/platform/renderer_rt64.cpp) renders at exactly that
// multiple and draws every game pixel as an n x n block, without resampling. CONKER_ORIGINAL_SIZE=1
// keeps the original 960x720 window instead, which is quicker for test runs.
#include "platform/window.hpp"

#include "platform/input_sdl.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include <SDL.h>
#include <SDL_syswm.h>

namespace conker::window {

namespace {

SDL_Window* g_window = nullptr;

// The picture RT64 presents. Conker's VI shows a 292-pixel-wide frame buffer (VI_WIDTH 292,
// VI_X_SCALE 0.5 over H_START 0x8C..0x2D5) and 214 lines (V_START 0x3D..0x1E8), inside black
// borders that RT64 crops (removeBlackBorders). RT64's VI::fbSize rounds the height up to 216.
constexpr int kPictureWidth = 292;
constexpr int kPictureHeight = 216;

constexpr int kOriginalWidth = 960;  // the window used before it was sized to the display
constexpr int kOriginalHeight = 720;

// Room for the title bar (28 points on macOS), which is not part of the window size.
constexpr int kTitleBarAllowance = 32;

// The largest multiple of the picture that fits the usable area (without the menu bar and Dock)
// of the primary display, where the centred window opens.
int fit_scale() {
    SDL_Rect usable;
    if (SDL_GetDisplayUsableBounds(0, &usable) != 0) {
        std::fprintf(stderr, "[conker] SDL_GetDisplayUsableBounds failed: %s\n", SDL_GetError());
        return 0;
    }
    const int scale = std::min(usable.w / kPictureWidth, (usable.h - kTitleBarAllowance) / kPictureHeight);
    std::printf("[conker] display %dx%d usable, window %dx%d (%dx the %dx%d picture)\n", usable.w, usable.h,
                kPictureWidth * scale, kPictureHeight * scale, scale, kPictureWidth, kPictureHeight);
    return scale;
}

bool original_size() {
    const char* value = std::getenv("CONKER_ORIGINAL_SIZE");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}

} // namespace

void* create_gfx() {
    SDL_SetHint(SDL_HINT_VIDEO_HIGHDPI_DISABLED, "1");
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
    int width = kOriginalWidth;
    int height = kOriginalHeight;
    if (original_size()) {
        std::printf("[conker] CONKER_ORIGINAL_SIZE: window %dx%d\n", width, height);
    } else if (const int scale = fit_scale(); scale > 0) {
        width = kPictureWidth * scale;
        height = kPictureHeight * scale;
    }
    g_window = SDL_CreateWindow("Conker's Bad Fur Day: Recompiled", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                width, height, flags);
    if (g_window == nullptr) {
        std::fprintf(stderr, "[conker] SDL_CreateWindow failed: %s\n", SDL_GetError());
        return {};
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
        conker::input::handle_event(event);
    }
    conker::input::update();
}

} // namespace conker::window
