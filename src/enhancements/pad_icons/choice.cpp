// Which button pictures to show (src/enhancements/pad_icons/pad_icons.cpp draws them): by the pad
// connected, or as CONKER_PAD_ICONS says, and the debug menu's switch for them. Kept apart from the
// drawing, which needs RT64, so that every build has the switch.
#include "enhancements/pad_icons/pad_icons.hpp"

#include <atomic>
#include <cstdlib>
#include <string>

#include "enhancements/switches.hpp"
#include "platform/input.hpp"

namespace conker::pad_icons {

namespace {

std::string setting() {
    const char* value = std::getenv("CONKER_PAD_ICONS");
    return value != nullptr ? value : "";
}

// CONKER_PAD_ICONS=xbox, playstation or n64, read once: that set whatever the pad, else -1.
int chosen() {
    static const int choice = [] {
        const std::string text = setting();
        if (text == "n64") return int(Style::N64);
        if (text == "xbox") return int(Style::Xbox);
        if (text == "playstation") return int(Style::PlayStation);
        return -1;
    }();
    return choice;
}

std::atomic<bool> g_on{setting() != "0"};  // the debug menu switches it

} // namespace

Style style() {
    if (!g_on.load(std::memory_order_relaxed)) return Style::Original;
    if (chosen() >= 0) return Style(chosen());
    switch (input::controller_kind()) {
        case input::PadKind::Xbox: return Style::Xbox;
        case input::PadKind::PlayStation: return Style::PlayStation;
        default: return Style::N64;
    }
}

const char* name(Style style) {
    switch (style) {
        case Style::Xbox: return "Xbox";
        case Style::PlayStation: return "PlayStation";
        case Style::N64: return "N64";
        default: return "original";
    }
}

} // namespace conker::pad_icons

conker::switches::Switch conker::switches::pad_buttons() {
    return {"PAD BUTTONS", [] { return conker::pad_icons::g_on.load(std::memory_order_relaxed); },
            [](bool on) { conker::pad_icons::g_on.store(on, std::memory_order_relaxed); }};
}
