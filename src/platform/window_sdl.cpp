// SDL2 window for the RT64 renderer. ultramodern calls create_gfx and create_window on the main
// thread before the game starts and update_gfx from the main loop every millisecond, so this is
// also where SDL events are pumped and controller 1 is sampled (macOS requires all of it on the
// main thread).
#include "platform/window.hpp"

#include "platform/input_sdl.hpp"

#include <cstdio>

#include <SDL.h>
#include <SDL_syswm.h>

namespace conker::window {

namespace {

SDL_Window* g_window = nullptr;

constexpr int kWindowWidth = 960;
constexpr int kWindowHeight = 720;

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
    g_window = SDL_CreateWindow("Conker's Bad Fur Day: Recompiled", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                kWindowWidth, kWindowHeight, flags);
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
