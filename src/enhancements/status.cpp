// What each enhancement is set to, for the debug menu's report: see src/enhancements/status.hpp.
#include "enhancements/status.hpp"

#include <algorithm>

#include "platform/display_mode.hpp"
#include "platform/input.hpp"
#include "platform/render_debug.hpp"

namespace conker::status {

namespace {

constexpr const char* kSharpFont = "built-in sharp font";

}  // namespace

std::vector<Line> enhancements(uint8_t* rdram) {
    const std::vector<std::string> packs = conker::renderer::texture_packs();
    const bool sharp_font = std::find(packs.begin(), packs.end(), kSharpFont) != packs.end();
    return {
        display_options(),
        intro_skip(),
        cutscene_skip(),
        pause_save(),
        save_snapshots(),
        surround_default(rdram),
        max_detail(),
        rumble_banner(),
        {"Sharper text", sharp_font, "CONKER_SHARP_FONT; the font enlarged with Super-xBR"},
        {"Camera on the right stick", true, "always on; C-left and C-right swapped, so left turns the view left"},
        debug_menu(),
    };
}

std::vector<std::pair<std::string, std::string>> settings(uint8_t* /*rdram*/) {
    std::vector<std::pair<std::string, std::string>> out;
    const conker::display::Mode mode = conker::display::current();
    const bool windowed = mode == conker::display::Mode::Windowed;
    const int scale = windowed ? conker::display::window_scale() : conker::display::screen_scale();
    const bool wide = conker::display::widescreen(mode);
    const int width = windowed ? conker::display::window_picture_width(scale) : conker::display::screen_picture_width(scale);
    out.emplace_back("display", std::string(conker::display::name(mode)) + ", " + (wide ? "widescreen" : "4:3") + ", " +
                                    std::to_string(scale) + "x (" + std::to_string(width) + "x" +
                                    std::to_string(conker::display::kPictureHeight * scale) + "), " +
                                    (conker::display::fill(mode) ? "fill screen" : "centered"));
    std::string loaded;
    for (const std::string& pack : conker::renderer::texture_packs()) {
        loaded += (loaded.empty() ? "" : ", ") + pack;
    }
    out.emplace_back("texture_packs", loaded.empty() ? "none" : loaded);
    const std::string pad = conker::input::controller_name();
    out.emplace_back("controller", (pad.empty() ? std::string("keyboard only") : pad) +
                                       (conker::input::has_rumble() ? ", can rumble" : ", no rumble"));
    return out;
}

}  // namespace conker::status
