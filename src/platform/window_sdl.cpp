// SDL2 window for the RT64 renderer. ultramodern calls create_gfx and create_window on the main
// thread before the game starts and update_gfx from the main loop every millisecond, so this is
// also where SDL events are pumped and controller 1 is sampled (macOS requires all of it on the
// main thread).
//
// The window opens at the largest whole multiple of the game's picture that fits the display, so
// RT64 (which scales by the window height, src/platform/renderer_rt64.cpp) renders at exactly that
// multiple and draws every game pixel as an n x n block, without resampling. CONKER_ORIGINAL_SIZE=1
// keeps the original 960x720 window (in points) instead, which is quicker for test runs.
//
// Sizes are in pixels. SDL places windows in points, which on a Retina screen are 2x2 pixels, and
// RT64's swap chain is in pixels (plume's Retina support, see CMakeLists.txt), so a window n times
// the picture is n x 292x216 pixels, n x 146x108 points on a 2x screen. The pixels per point are
// measured on the window itself, and the limits and window size follow it to another display.
//
// The display mode (src/platform/display_mode.hpp) is full screen unless the OPTIONS menu or
// CONKER_DISPLAY says otherwise: macOS full screen in a Space of its own, a borderless window
// covering the screen in the current Space, or the window above. F11, Cmd+Enter, Alt+Enter or
// Ctrl+Cmd+F switch between the window and the last screen-covering mode. Covering the screen,
// RT64 renders at the multiple that covers the screen height and shrinks it slightly to fit (7x on
// a 1440-line display). In widescreen the window is 16:9 (kWidePictureWidth wide for each multiple)
// and RT64 widens the picture to the window's or screen's shape (src/platform/renderer_rt64.cpp).
#include "platform/window.hpp"

#include "enhancements/object_inspector/object_inspector.hpp"
#include "platform/display_mode.hpp"
#include "platform/hang_watch.hpp"
#include "platform/input_sdl.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <SDL.h>
#include <SDL_syswm.h>

#if defined(__APPLE__)
#include <objc/message.h>
#include <objc/runtime.h>
#elif defined(__linux__)
#include <filesystem>
#include "stb/stb_image.h"  // implemented in RT64 (rt64_texture_cache.cpp)
#endif

namespace conker::window {

namespace {

// The picture RT64 presents. Conker's VI shows a 292-pixel-wide frame buffer (VI_WIDTH 292,
// VI_X_SCALE 0.5 over H_START 0x8C..0x2D5) and 214 lines (V_START 0x3D..0x1E8), inside black
// borders that RT64 crops (removeBlackBorders). RT64's VI::fbSize rounds the height up to 216.
constexpr int kPictureWidth = conker::display::kPictureWidth;
constexpr int kPictureHeight = conker::display::kPictureHeight;
constexpr int kWidePictureWidth = conker::display::kWidePictureWidth;

constexpr int kOriginalWidth = 960;  // the window used before it was sized to the display
constexpr int kOriginalWideWidth = 1280;  // and its 16:9 counterpart
constexpr int kOriginalHeight = 720;

// Room for the title bar (28 points on macOS), which is not part of the window size.
constexpr int kTitleBarAllowance = 32;

SDL_Window* g_window = nullptr;
int g_display = 0;       // the display the window is on
double g_density = 1.0;  // its pixels per point: 2 on a Retina screen

// The window's pixels per point, from its size in each.
double measure_density() {
    int w, h, pixel_w, pixel_h;
    SDL_GetWindowSize(g_window, &w, &h);
    SDL_GetWindowSizeInPixels(g_window, &pixel_w, &pixel_h);
    return w > 0 && pixel_w > 0 ? static_cast<double>(pixel_w) / w : 1.0;
}

int to_pixels(int points) { return static_cast<int>(points * g_density); }
int to_points(int pixels) { return static_cast<int>(std::lround(pixels / g_density)); }

// The largest multiples of the picture that fit the usable area (without the menu bar and Dock)
// of the window's display, and the whole display.
void find_limits() {
    SDL_Rect usable;
    SDL_Rect bounds;
    if (SDL_GetDisplayUsableBounds(g_display, &usable) != 0 || SDL_GetDisplayBounds(g_display, &bounds) != 0) {
        std::fprintf(stderr, "[conker] SDL_GetDisplayBounds failed: %s\n", SDL_GetError());
        return;
    }
    const int usable_w = to_pixels(usable.w);
    const int usable_h = to_pixels(usable.h - kTitleBarAllowance);
    const int screen_w = to_pixels(bounds.w);
    const int screen_h = to_pixels(bounds.h);
    conker::display::Limits limits;
    limits.window_max = std::min(usable_w / kPictureWidth, usable_h / kPictureHeight);
    limits.wide_window_max = std::min(usable_w / kWidePictureWidth, usable_h / kPictureHeight);
    // The smallest multiple covering the screen height (RT64 shrinks it to fit), unless the width
    // already limits it. A widescreen picture takes the screen's shape, so only its height counts.
    const int cover_height = (screen_h + kPictureHeight - 1) / kPictureHeight;
    limits.screen_max = std::min((screen_w + kPictureWidth - 1) / kPictureWidth, cover_height);
    limits.wide_screen_max = cover_height;
    limits.screen_width = screen_w;
    limits.screen_height = screen_h;
    conker::display::set_limits(limits);
    std::printf("[conker] display %dx%d pixels (%g per point), %dx%d usable: up to %dx in a window (%dx in "
                "widescreen), %dx covering the screen (%dx)\n",
                screen_w, screen_h, g_density, to_pixels(usable.w), to_pixels(usable.h), limits.window_max,
                limits.wide_window_max, limits.screen_max, limits.wide_screen_max);
}

bool original_size() {
    const char* value = std::getenv("CONKER_ORIGINAL_SIZE");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}

using conker::display::Mode;

Mode g_screen_mode = Mode::Fullscreen;  // where the shortcuts go from the window

// The window's frame in points when it is a window, centred on its display.
SDL_Rect window_frame() {
    const int centered = static_cast<int>(SDL_WINDOWPOS_CENTERED_DISPLAY(g_display));
    SDL_Rect frame{centered, centered,
                   conker::display::widescreen(Mode::Windowed) ? kOriginalWideWidth : kOriginalWidth, kOriginalHeight};
    if (!original_size()) {
        frame.w = to_points(conker::display::window_picture_width(conker::display::window_scale()));
        frame.h = to_points(kPictureHeight * conker::display::window_scale());
    }
    return frame;
}

// Leaving a full-screen Space is animated, and when it ends macOS puts the window back at the frame
// it had before, after apply_mode has set the new one; coming back from the borderless frame, it is
// also left maximized, which makes SDL ignore new sizes. So for a while after a change the window is
// un-maximized and given the frame the mode wants whenever it has another.
constexpr Uint64 kSettleMs = 3000;
SDL_Rect g_wanted_frame{};  // x and y SDL_WINDOWPOS_CENTERED_DISPLAY for the window
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
    const bool centered = SDL_WINDOWPOS_ISCENTERED(g_wanted_frame.x);
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
    SDL_ShowCursor(mode == Mode::Windowed || conker::inspector::active() ? SDL_ENABLE : SDL_DISABLE);
    if (mode != Mode::Windowed) {
        g_screen_mode = mode;
    }
}

// RT64 keeps rendering as it was while the window changes mode (conker::display::window_changing):
// until the window has the mode's size, at least kMinChangeMs after apply_mode, and kSettleMs at most.
constexpr Uint64 kMinChangeMs = 250;
bool g_changing = false;
uint32_t g_changing_requests = 0;  // conker::display::mode_requests() when the change was taken
Uint64 g_changed_at = 0;

bool has_mode_size() {
    const bool full = (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN) != 0;
    int w, h;
    SDL_GetWindowSize(g_window, &w, &h);
    if (conker::display::current() == Mode::Fullscreen) {
        SDL_Rect bounds;
        return full && SDL_GetDisplayBounds(std::max(SDL_GetWindowDisplayIndex(g_window), 0), &bounds) == 0 &&
               w == bounds.w && h == bounds.h;
    }
    return !full && w == g_wanted_frame.w && h == g_wanted_frame.h;
}

void settle_mode_change() {
    if (!g_changing) {
        return;
    }
    const Uint64 now = SDL_GetTicks64();
    if (now < g_changed_at + kMinChangeMs || (now < g_changed_at + kSettleMs && !has_mode_size())) {
        return;
    }
    g_changing = false;
    conker::display::set_window_settled(g_changing_requests);
}

// Moved to another display, or the display's pixels per point changed: new limits, and a window
// that still has the size it was given is resized to stay exact in pixels, keeping its centre.
void follow_display() {
    const int display = std::max(SDL_GetWindowDisplayIndex(g_window), 0);
    const double density = measure_density();
    if (display == g_display && density == g_density) {
        return;
    }
    int x, y, w, h;
    SDL_GetWindowPosition(g_window, &x, &y);
    SDL_GetWindowSize(g_window, &w, &h);
    const SDL_Rect old_frame = window_frame();
    g_display = display;
    g_density = density;
    find_limits();
    const bool full = (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN) != 0;
    if (original_size() || conker::display::current() != Mode::Windowed || full || w != old_frame.w ||
        h != old_frame.h) {
        return;
    }
    SDL_Rect frame = window_frame();
    if (frame.w == w && frame.h == h) {
        return;
    }
    frame.x = x + (w - frame.w) / 2;
    frame.y = y + (h - frame.h) / 2;
    g_wanted_frame = frame;
    set_frame(frame);
    std::printf("[conker] window %dx%d pixels\n", to_pixels(frame.w), to_pixels(frame.h));
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

// CONKER_DISPLAY_SWITCH=<ms>:<mode>[,...] asks for a display mode (windowed, borderless or
// fullscreen) that many milliseconds after start, as the shortcuts would, so mode changes can be
// tested by script (docs/diagnostics.md).
struct ScheduledSwitch {
    Uint64 at_ms;
    Mode mode;
};

std::vector<ScheduledSwitch> scheduled_switches() {
    std::vector<ScheduledSwitch> switches;
    const char* value = std::getenv("CONKER_DISPLAY_SWITCH");
    for (const char* p = value; p != nullptr && *p != '\0';) {
        char* end = nullptr;
        const unsigned long long ms = std::strtoull(p, &end, 10);
        if (end == p || *end != ':') {
            break;
        }
        const char* name = end + 1;
        const char* comma = std::strchr(name, ',');
        const size_t length = comma != nullptr ? static_cast<size_t>(comma - name) : std::strlen(name);
        for (int i = 0; i < conker::display::kModeCount; i++) {
            const char* mode_name = conker::display::name(static_cast<Mode>(i));
            if (std::strlen(mode_name) == length && std::strncmp(mode_name, name, length) == 0) {
                switches.push_back({ms, static_cast<Mode>(i)});
            }
        }
        p = comma != nullptr ? comma + 1 : name + length;
    }
    return switches;
}

void run_scheduled_switches() {
    static std::vector<ScheduledSwitch> switches = scheduled_switches();
    static size_t next = 0;
    if (next < switches.size() && SDL_GetTicks64() >= switches[next].at_ms) {
        std::printf("[conker] CONKER_DISPLAY_SWITCH: %s\n", conker::display::name(switches[next].mode));
        conker::display::request(switches[next].mode);
        next++;
    }
}

} // namespace

#if defined(__linux__)
// The window's icon: conkers-bad-fur-day-recompiled.png beside the executable (tools/linux/build.sh
// puts it there).
// The desktop's own icon for the game comes from the .desktop file install.sh writes, matched to
// the window by its class, which SDL names after the executable: ConkersBadFurDayRecompiled.
static void set_window_icon() {
    std::error_code error;
    const std::filesystem::path exe = std::filesystem::read_symlink("/proc/self/exe", error);
    if (error) {
        return;
    }
    const std::string path = (exe.parent_path() / "conkers-bad-fur-day-recompiled.png").string();
    int width = 0, height = 0, channels = 0;
    unsigned char* pixels = stbi_load(path.c_str(), &width, &height, &channels, 4);
    if (pixels == nullptr) {
        return;
    }
    SDL_Surface* icon = SDL_CreateRGBSurfaceWithFormatFrom(pixels, width, height, 32, width * 4, SDL_PIXELFORMAT_RGBA32);
    if (icon != nullptr) {
        SDL_SetWindowIcon(g_window, icon);
        SDL_FreeSurface(icon);
    }
    stbi_image_free(pixels);
}
#endif

void* create_gfx() {
    // SDL 3's SDL_HINT_VIDEO_SYNC_WINDOW_OPERATIONS (through sdl2-compat): leaving full screen
    // finishes before the next window change, so a borderless window is not resized mid-animation.
    SDL_SetHint("SDL_VIDEO_SYNC_WINDOW_OPERATIONS", "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        std::fprintf(stderr, "[conker] SDL_Init failed: %s\n", SDL_GetError());
        return nullptr;
    }
    conker::input::init();
    conker::hang_watch::start();
    return &g_window;
}

ultramodern::renderer::WindowHandle create_window(void* /*gfx_data*/) {
    Uint32 flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_HIDDEN;
#if defined(__APPLE__)
    flags |= SDL_WINDOW_METAL;
#elif defined(__linux__)
    flags |= SDL_WINDOW_VULKAN;
#endif
    // Hidden until it is measured: the size in points depends on the display's pixels per point.
    g_window = SDL_CreateWindow("Conker's Bad Fur Day Recompiled", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                kOriginalWidth, kOriginalHeight, flags);
    if (g_window == nullptr) {
        std::fprintf(stderr, "[conker] SDL_CreateWindow failed: %s\n", SDL_GetError());
        return {};
    }
#if defined(__linux__)
    set_window_icon();
#endif
    g_display = std::max(SDL_GetWindowDisplayIndex(g_window), 0);
    g_density = measure_density();
    find_limits();
    const Mode saved = conker::display::startup_mode();
    const Mode mode = original_size() ? Mode::Windowed : saved;
    const SDL_Rect frame = window_frame();
    if (original_size()) {
        std::printf("[conker] CONKER_ORIGINAL_SIZE: window %dx%d\n", frame.w, frame.h);
    }
    set_frame(frame);
    SDL_ShowWindow(g_window);
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

namespace {

// The mouse, for the object inspector (src/enhancements/object_inspector): its place as a fraction of
// the window, clicks and the wheel (down, or towards you, looks further behind). The cursor shows on
// a full screen too while the inspector is on.
void follow_mouse(const SDL_Event& event) {
    const Uint32 window = SDL_GetWindowID(g_window);
    int w, h;
    SDL_GetWindowSize(g_window, &w, &h);
    if (event.type == SDL_MOUSEMOTION && event.motion.windowID == window && w > 0 && h > 0) {
        conker::inspector::mouse_moved(float(event.motion.x) / w, float(event.motion.y) / h);
    } else if (event.type == SDL_MOUSEBUTTONDOWN && event.button.windowID == window && w > 0 && h > 0) {
        conker::inspector::mouse_moved(float(event.button.x) / w, float(event.button.y) / h);
        conker::inspector::mouse_clicked(event.button.button == SDL_BUTTON_LEFT ? 1 : event.button.button == SDL_BUTTON_RIGHT ? 3 : 0);
    } else if (event.type == SDL_MOUSEWHEEL && event.wheel.windowID == window) {
        const int steps = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? event.wheel.y : -event.wheel.y;
        conker::inspector::mouse_wheel(steps);
    } else if (event.type == SDL_WINDOWEVENT && event.window.windowID == window && event.window.event == SDL_WINDOWEVENT_LEAVE) {
        conker::inspector::mouse_left();
    }
}

void show_cursor_for_inspector() {
    static bool shown = false;
    const bool wanted = conker::inspector::active();
    if (wanted != shown) {
        shown = wanted;
        SDL_ShowCursor(wanted || conker::display::current() == Mode::Windowed ? SDL_ENABLE : SDL_DISABLE);
    }
}

} // namespace

void update_gfx(void* /*gfx_data*/) {
    conker::hang_watch::beat();
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        follow_mouse(event);
        if (event.type == SDL_QUIT) {
            ultramodern::quit();
        }
        if (event.type == SDL_WINDOWEVENT && event.window.windowID == SDL_GetWindowID(g_window)) {
            follow_display();
        }
        if (event.type == SDL_KEYDOWN && is_fullscreen_toggle(event.key)) {
            conker::display::request(conker::display::current() == Mode::Windowed ? g_screen_mode : Mode::Windowed);
        }
        conker::input::handle_event(event);
    }
    run_scheduled_switches();
    if (Mode mode; conker::display::take_request(&mode)) {
        g_changing_requests = conker::display::mode_requests();
        apply_mode(mode);
        g_changing = true;
        g_changed_at = SDL_GetTicks64();
    }
    settle_mode_change();
    show_cursor_for_inspector();
    keep_frame();
    conker::input::update();
}

} // namespace conker::window
