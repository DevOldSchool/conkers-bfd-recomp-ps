// The debug menu's switches: see src/enhancements/switches.hpp. The display settings and the camera
// stick live in src/platform, so their switches are here.
#include "enhancements/switches.hpp"

#include "platform/display_mode.hpp"
#include "platform/input.hpp"

namespace conker::switches {

namespace {

Switch sharp() {
    return {"SHARP TEXT", conker::display::sharp, conker::display::request_sharp};
}

// The current mode's shape, as the OPTIONS row sets it.
Switch widescreen() {
    return {"WIDESCREEN", [] { return conker::display::widescreen(conker::display::current()); },
            [](bool on) { conker::display::request_widescreen(conker::display::current(), on); }};
}

Switch camera_stick() {
    return {"CAMERA STICK SWAP", conker::input::camera_swapped, conker::input::set_camera_swapped};
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
        save_snapshots(),
        surround_default(),
        max_detail(),
        draw_distance(),
        rumble_banner(),
        haptics(),
        camera_stick(),
        pad_buttons(),
        frame_rate(),
    };
}

}  // namespace conker::switches
