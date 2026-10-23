// What each enhancement is set to, the settings that go with them, and the computer the game runs
// on, as JSON for the debug menu's report (src/enhancements/debug_menu). Each enhancement answers for
// itself, from the switch it acts on.
#pragma once

#include <cstdint>

#include "json/json.hpp"

namespace conker::status {

// One enhancement: {"id", "name" (as the README calls it), "on", "variable" (the CONKER_* variable
// that switches it, or null), "options" (what it is set to, as values)}.
nlohmann::json enhancement(const char* id, const char* name, bool on, const char* variable,
                           nlohmann::json options = nlohmann::json::object());

// Every enhancement, in the README's order.
nlohmann::json enhancements(uint8_t* rdram);

// {"display", "texture_packs", "controller"}.
nlohmann::json settings();

// {"os", "cpu", "memory_bytes", "gpu", "display"}: nothing that names the computer or its user.
nlohmann::json system();

// Each enhancement's own entry (defined in its file).
nlohmann::json cutscene_skip();
nlohmann::json intro_skip();
nlohmann::json max_detail();
nlohmann::json character_distance();
nlohmann::json frame_rate();
nlohmann::json rumble_banner();
nlohmann::json haptics();
nlohmann::json surround_default(uint8_t* rdram);
nlohmann::json display_options();
nlohmann::json pause_save();
nlohmann::json save_snapshots();
nlohmann::json debug_menu();

}  // namespace conker::status
