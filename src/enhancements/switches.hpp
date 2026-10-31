// The enhancements the debug menu's ENHANCEMENTS page turns on and off while the game runs
// (src/enhancements/debug_menu). Each starts as its CONKER_* variable says and stays as the page
// leaves it until the game is closed. Each enhancement answers for itself, from the switch it acts
// on, as for conker::status (src/enhancements/status.hpp).
#pragma once

#include <vector>

namespace conker::switches {

struct Switch {
    const char* label;  // as the page shows it
    bool (*on)();
    void (*set)(bool on);
    // This machine's own: it writes files, so another machine's settings (an online host's) do not set
    // it. A replay's header, a file of this machine's, still does.
    bool local = false;
};

// Every switch, in the README's order: the display options' SHARP TEXT and WIDESCREEN (the OPTIONS
// rows' own settings, kept in display.cfg as those rows keep them) and the enhancements. Not the
// OPTIONS rows themselves, which the menu engine is steering through, nor the debug menu.
std::vector<Switch> all();

// Each enhancement's own switch (defined in its file).
Switch widescreen_culling();
Switch intro_skip();
Switch cutscene_skip();
Switch pause_save();
Switch save_snapshots();
Switch surround_default();
Switch max_detail();
Switch draw_distance();
Switch haptics();
Switch adaptive_triggers();
Switch consistent_controls();
Switch frame_rate();
Switch pad_buttons();  // src/enhancements/pad_icons/choice.cpp

}  // namespace conker::switches
