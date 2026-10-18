#include "platform/display_mode.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

namespace conker::display {

namespace {

std::atomic<int> g_current{static_cast<int>(Mode::Fullscreen)};
std::atomic<bool> g_requested{false};

// Next to the saves, under the working directory (see main.cpp).
std::filesystem::path config_path() {
    return std::filesystem::current_path() / "conker-recomp-data" / "display.cfg";
}

bool parse(const char* text, Mode* mode) {
    for (int i = 0; i < kModeCount; i++) {
        if (std::strcmp(text, name(static_cast<Mode>(i))) == 0) {
            *mode = static_cast<Mode>(i);
            return true;
        }
    }
    return false;
}

void save(Mode mode) {
    std::ofstream file(config_path());
    file << name(mode) << '\n';
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
    if (const char* value = std::getenv("CONKER_DISPLAY"); value != nullptr && value[0] != '\0') {
        if (!parse(value, &mode)) {
            std::fprintf(stderr, "[conker] CONKER_DISPLAY=%s: expected windowed, borderless or fullscreen\n", value);
        }
    } else {
        std::ifstream file(config_path());
        std::string line;
        if (std::getline(file, line) && !parse(line.c_str(), &mode)) {
            std::fprintf(stderr, "[conker] %s: unknown mode '%s'\n", config_path().string().c_str(), line.c_str());
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
    save(mode);
    g_requested.store(true);
}

bool take_request(Mode* mode) {
    if (!g_requested.exchange(false)) {
        return false;
    }
    *mode = current();
    return true;
}

} // namespace conker::display
