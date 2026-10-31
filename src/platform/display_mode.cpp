#include "platform/display_mode.hpp"

#include <algorithm>
#include <cmath>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

namespace conker::display {

namespace {

std::atomic<int> g_current{static_cast<int>(Mode::Fullscreen)};
std::atomic<int> g_window_scale{0};  // 0 until set_limits: the largest that fits
std::atomic<int> g_screen_scale{0};  // 0: the largest
std::atomic<bool> g_window_fill{false};
std::atomic<bool> g_screen_fill{true};
std::atomic<bool> g_window_wide{false};
std::atomic<bool> g_screen_wide{true};
std::atomic<int> g_window_max{kMinScale};
std::atomic<int> g_wide_window_max{kMinScale};
std::atomic<int> g_screen_max{kMinScale};
std::atomic<int> g_wide_screen_max{kMinScale};
std::atomic<int> g_screen_width{0};  // 0 until set_limits
std::atomic<int> g_screen_height{0};
std::atomic<bool> g_requested{false};
std::atomic<uint32_t> g_mode_requests{0};
std::atomic<uint32_t> g_settled_requests{0};  // g_mode_requests when the window last settled
std::atomic<uint32_t> g_frame_copies{0};
std::atomic<bool> g_rendered_widescreen{false};
std::atomic<bool> g_sharp{true};
std::atomic<float> g_rendered_aspect{float(kPictureWidth) / kPictureHeight};
std::mutex g_save_mutex;
// keep_saved_picture: the widescreen and sharp settings save() writes in place of this run's.
std::atomic<bool> g_keep_saved{false};
bool g_saved_window_wide = false;
bool g_saved_screen_wide = true;
bool g_saved_sharp = true;

// Next to the saves, under the working directory (see main.cpp).
std::filesystem::path config_path() {
    return std::filesystem::current_path() / "conker-recomp-data" / "display.cfg";
}

bool parse_mode(const std::string& text, Mode* mode) {
    for (int i = 0; i < kModeCount; i++) {
        if (text == name(static_cast<Mode>(i))) {
            *mode = static_cast<Mode>(i);
            return true;
        }
    }
    return false;
}

// display.cfg holds "mode=<name>", "window_scale=<n>", "screen_scale=<n>", "window_fill=<0|1>",
// "screen_fill=<0|1>", "window_widescreen=<0|1>", "screen_widescreen=<0|1>" and "sharp=<0|1>"
// lines; a scale of 0 is the largest.
void save() {
    std::lock_guard lock(g_save_mutex);
    const bool keep = g_keep_saved.load();
    const bool window_wide = keep ? g_saved_window_wide : g_window_wide.load();
    const bool screen_wide = keep ? g_saved_screen_wide : g_screen_wide.load();
    const bool sharp = keep ? g_saved_sharp : g_sharp.load();
    std::ofstream file(config_path());
    file << "mode=" << name(current()) << '\n'
         << "window_scale=" << g_window_scale.load() << '\n'
         << "screen_scale=" << g_screen_scale.load() << '\n'
         << "window_fill=" << (g_window_fill.load() ? 1 : 0) << '\n'
         << "screen_fill=" << (g_screen_fill.load() ? 1 : 0) << '\n'
         << "window_widescreen=" << (window_wide ? 1 : 0) << '\n'
         << "screen_widescreen=" << (screen_wide ? 1 : 0) << '\n'
         << "sharp=" << (sharp ? 1 : 0) << '\n';
    if (!file) {
        std::fprintf(stderr, "[conker] could not write %s\n", config_path().string().c_str());
    }
}

} // namespace

const char* name(Mode mode) {
    switch (mode) {
        case Mode::Windowed: return "windowed";
        case Mode::Borderless: return "borderless";
        case Mode::Fullscreen: return "fullscreen";
    }
    return "fullscreen";
}

Mode startup_mode() {
    Mode mode = Mode::Fullscreen;
    std::ifstream file(config_path());
    for (std::string line; std::getline(file, line);) {
        const size_t equals = line.find('=');
        const std::string key = equals == std::string::npos ? "mode" : line.substr(0, equals);
        const std::string value = equals == std::string::npos ? line : line.substr(equals + 1);
        if (key == "mode") {
            if (!parse_mode(value, &mode)) {
                std::fprintf(stderr, "[conker] %s: unknown mode '%s'\n", config_path().string().c_str(), value.c_str());
            }
        } else if (key == "window_scale") {
            g_window_scale.store(std::atoi(value.c_str()));
        } else if (key == "screen_scale") {
            g_screen_scale.store(std::atoi(value.c_str()));
        } else if (key == "window_fill") {
            g_window_fill.store(std::atoi(value.c_str()) != 0);
        } else if (key == "screen_fill") {
            g_screen_fill.store(std::atoi(value.c_str()) != 0);
        } else if (key == "window_widescreen") {
            g_window_wide.store(std::atoi(value.c_str()) != 0);
        } else if (key == "screen_widescreen") {
            g_screen_wide.store(std::atoi(value.c_str()) != 0);
        } else if (key == "sharp") {
            g_sharp.store(std::atoi(value.c_str()) != 0);
        }
    }
    if (const char* value = std::getenv("CONKER_DISPLAY"); value != nullptr && value[0] != '\0') {
        if (!parse_mode(value, &mode)) {
            std::fprintf(stderr, "[conker] CONKER_DISPLAY=%s: expected windowed, borderless or fullscreen\n", value);
        }
    }
    return mode;
}

void started(Mode mode) { g_current.store(static_cast<int>(mode)); }

Mode current() { return static_cast<Mode>(g_current.load()); }

void request(Mode mode) {
    if (g_current.exchange(static_cast<int>(mode)) == static_cast<int>(mode)) {
        return;
    }
    std::printf("[conker] display mode: %s\n", name(mode));
    save();
    g_mode_requests.fetch_add(1);  // before the window can take the request
    g_requested.store(true);
}

bool window_changing() { return g_settled_requests.load() != g_mode_requests.load(); }

uint32_t mode_requests() { return g_mode_requests.load(); }

void set_window_settled(uint32_t requests) { g_settled_requests.store(requests); }

void set_limits(const Limits& limits) {
    g_window_max.store(std::max(limits.window_max, kMinScale));
    g_wide_window_max.store(std::max(limits.wide_window_max, kMinScale));
    g_screen_max.store(std::max(limits.screen_max, kMinScale));
    g_wide_screen_max.store(std::max(limits.wide_screen_max, kMinScale));
    g_screen_width.store(limits.screen_width);
    g_screen_height.store(limits.screen_height);
}

int display_width() { return g_screen_width.load(); }
int display_height() { return g_screen_height.load(); }

int window_max() { return (widescreen(Mode::Windowed) ? g_wide_window_max : g_window_max).load(); }
int screen_max() { return (widescreen(Mode::Fullscreen) ? g_wide_screen_max : g_screen_max).load(); }

bool widescreen(Mode mode) { return (mode == Mode::Windowed ? g_window_wide : g_screen_wide).load(); }

int window_picture_width(int scale) {
    return (widescreen(Mode::Windowed) ? kWidePictureWidth : kPictureWidth) * scale;
}

int screen_picture_width(int scale) {
    const int width = g_screen_width.load();
    const int height = g_screen_height.load();
    if (!widescreen(Mode::Fullscreen) || width <= 0 || height <= 0) {
        return kPictureWidth * scale;
    }
    // RT64 widens the picture to the screen's shape, never narrower than 4:3.
    const long wide = std::lround(static_cast<double>(kPictureHeight) * scale * width / height);
    return std::max(static_cast<int>(wide), kPictureWidth * scale);
}

int window_scale() {
    const int scale = g_window_scale.load();
    return scale < kMinScale ? window_max() : std::min(scale, window_max());
}

int screen_scale() {
    const int scale = g_screen_scale.load();
    return scale < kMinScale ? screen_max() : std::min(scale, screen_max());
}

bool fill(Mode mode) { return (mode == Mode::Windowed ? g_window_fill : g_screen_fill).load(); }

// The largest multiple is kept as 0, "the largest", so it follows the display: another display, a
// Retina one, or widescreen, which has its own limits.
void request_window_scale(int scale) {
    const int stored = scale >= window_max() ? 0 : scale;
    if (g_window_scale.exchange(stored) == stored) {
        return;
    }
    std::printf("[conker] window scale: %dx%s\n", scale, stored == 0 ? " (the largest)" : "");
    save();
    g_requested.store(true);
}

void request_screen_scale(int scale) {
    const int stored = scale >= screen_max() ? 0 : scale;
    if (g_screen_scale.exchange(stored) == stored) {
        return;
    }
    std::printf("[conker] screen scale: %dx%s\n", scale, stored == 0 ? " (the largest)" : "");
    save();
}

void request_fill(Mode mode, bool fill) {
    if ((mode == Mode::Windowed ? g_window_fill : g_screen_fill).exchange(fill) == fill) {
        return;
    }
    std::printf("[conker] %s picture: %s\n", mode == Mode::Windowed ? "window" : "screen", fill ? "fill" : "centered");
    save();
}

void request_widescreen(Mode mode, bool widescreen) {
    if ((mode == Mode::Windowed ? g_window_wide : g_screen_wide).exchange(widescreen) == widescreen) {
        return;
    }
    std::printf("[conker] %s picture: %s\n", mode == Mode::Windowed ? "window" : "screen",
                widescreen ? "widescreen" : "4:3");
    save();
    if (mode == Mode::Windowed && current() == Mode::Windowed) {
        g_requested.store(true);  // the window takes the new shape; covering the screen, only RT64 changes
    }
}

bool sharp() { return g_sharp.load(); }

void request_sharp(bool sharp) {
    if (g_sharp.exchange(sharp) == sharp) {
        return;
    }
    std::printf("[conker] sharper text and menus: %s\n", sharp ? "on" : "off");
    save();
}

void keep_saved_picture() {
    std::lock_guard lock(g_save_mutex);
    if (g_keep_saved.exchange(true)) {
        return;
    }
    g_saved_window_wide = g_window_wide.load();
    g_saved_screen_wide = g_screen_wide.load();
    g_saved_sharp = g_sharp.load();
}

void note_frame_copy() { g_frame_copies.fetch_add(1); }

uint32_t frame_copy_count() { return g_frame_copies.load(); }

void set_rendered_widescreen(bool widescreen) { g_rendered_widescreen.store(widescreen); }

bool rendered_widescreen() { return g_rendered_widescreen.load(); }

void set_rendered_aspect(float aspect) { g_rendered_aspect.store(aspect); }

float rendered_aspect() { return g_rendered_aspect.load(); }

namespace {
std::atomic<float> g_logic_aspect{0.0f};  // 0: the rendered one
}

void set_logic_aspect(float aspect) { g_logic_aspect.store(aspect); }

float logic_aspect() {
    const float aspect = g_logic_aspect.load();
    return aspect > 0.0f ? aspect : rendered_aspect();
}

bool take_request(Mode* mode) {
    if (!g_requested.exchange(false)) {
        return false;
    }
    *mode = current();
    return true;
}

} // namespace conker::display
