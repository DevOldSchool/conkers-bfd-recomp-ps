// What each enhancement is set to, and the settings that go with them, for the debug menu's report
// (src/enhancements/debug_menu). Each enhancement answers for itself, from the switch it acts on.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace conker::status {

struct Line {
    std::string name;    // as the README calls it
    bool on = false;
    std::string detail;  // the switch, and what it does now
};

// One line per enhancement.
std::vector<Line> enhancements(uint8_t* rdram);

// The settings that go with them, as name and value: the display, texture packs, the controller.
std::vector<std::pair<std::string, std::string>> settings(uint8_t* rdram);

// Each enhancement's own line (defined in its file).
Line cutscene_skip();
Line intro_skip();
Line max_detail();
Line rumble_banner();
Line surround_default(uint8_t* rdram);
Line display_options();
Line pause_save();
Line save_snapshots();
Line debug_menu();

}  // namespace conker::status
