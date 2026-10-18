#include "platform/display_mode.hpp"

#include <algorithm>
#include <atomic>
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
std::atomic<int> g_window_max{kMinScale};
std::atomic<int> g_screen_max{kMinScale};
std::atomic<bool> g_requested{false};
std::mutex g_save_mutex;

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

// display.cfg holds "mode=<name>", "window_scale=<n>", "screen_scale=<n>", "window_fill=<0|1>" and
// "screen_fill=<0|1>" lines; a scale of 0 is the largest.
void save() {
    std::lock_guard lock(g_save_mutex);
    std::ofstream file(config_path());
    file << "mode=" << name(current()) << '\n'
         << "window_scale=" << g_window_scale.load() << '\n'
         << "screen_scale=" << g_screen_scale.load() << '\n'
         << "window_fill=" << (g_window_fill.load() ? 1 : 0) << '\n'
         << "screen_fill=" << (g_screen_fill.load() ? 1 : 0) << '\n';
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
    g_requested.store(true);
}

void set_limits(int window_max_scale, int screen_max_scale) {
    g_window_max.store(std::max(window_max_scale, kMinScale));
    g_screen_max.store(std::max(screen_max_scale, kMinScale));
}

int window_max() { return g_window_max.load(); }
int screen_max() { return g_screen_max.load(); }

int window_scale() {
    const int scale = g_window_scale.load();
    return scale < kMinScale ? window_max() : std::min(scale, window_max());
}

int screen_scale() {
    const int scale = g_screen_scale.load();
    return scale < kMinScale ? screen_max() : std::min(scale, screen_max());
}

bool fill(Mode mode) { return (mode == Mode::Windowed ? g_window_fill : g_screen_fill).load(); }

void request_window_scale(int scale) {
    if (g_window_scale.exchange(scale) == scale) {
        return;
    }
    std::printf("[conker] window scale: %dx\n", scale);
    save();
    g_requested.store(true);
}

void request_screen_scale(int scale) {
    if (g_screen_scale.exchange(scale) == scale) {
        return;
    }
    std::printf("[conker] screen scale: %dx\n", scale);
    save();
}

void request_fill(Mode mode, bool fill) {
    if ((mode == Mode::Windowed ? g_window_fill : g_screen_fill).exchange(fill) == fill) {
        return;
    }
    std::printf("[conker] %s picture: %s\n", mode == Mode::Windowed ? "window" : "screen", fill ? "fill" : "centered");
    save();
}

bool take_request(Mode* mode) {
    if (!g_requested.exchange(false)) {
        return false;
    }
    *mode = current();
    return true;
}

} // namespace conker::display
