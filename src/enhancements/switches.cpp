// The debug menu's switches: see src/enhancements/switches.hpp. The display settings live in
// src/platform, so their switches are here.
#include "enhancements/switches.hpp"

#include "platform/display_mode.hpp"

namespace conker::switches {

namespace {

Switch sharp() {
    // Each player's own online: the renderer's font and menu pictures, and the font's TEXRECTs widened
    // for the renderer alone and given back (font_rects.hpp); the game's memory holds the same.
    return {"SHARP TEXT", conker::display::sharp, conker::display::request_sharp, true};
}

// The current mode's shape, as the OPTIONS row sets it.
Switch widescreen() {
    return {"WIDESCREEN", [] { return conker::display::widescreen(conker::display::current()); },
            [](bool on) { conker::display::request_widescreen(conker::display::current(), on); }};
}

}  // namespace

std::vector<Switch> all() {
    return {
        sharp(),
        widescreen(),
        widescreen_culling(),
        intro_skip(),
        cutscene_skip(),
        pause_save(),
        quit_game(),
        save_snapshots(),
        surround_default(),
        mp_characters(),
        max_detail(),
        draw_distance(),
        haptics(),
        adaptive_triggers(),
        consistent_controls(),
        pad_buttons(),
        frame_rate(),
        my_view(),
    };
}

}  // namespace conker::switches
