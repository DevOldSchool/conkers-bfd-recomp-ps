// The ONLINE lobby's look: the game's own font, button pictures and wood (the MULTI menus'), with
// cream and gold text and plates for buttons.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "imgui/imgui.h"

namespace conker::lobby_style {

// Before a frame, on the thread that builds it: makes the game's font once the game has loaded its
// character table (`rdram`), and the button pictures.
void prepare(const uint8_t* rdram);

// The style for the window's scale (1 at 720 lines).
void apply(float scale);

// The font to push for the window, or null for ImGui's.
ImFont* font();

// Colours for text: grey (hints), red (errors), green (good), yellow (waiting, warnings).
ImU32 grey();
ImU32 red();
ImU32 green();
ImU32 yellow();
ImU32 player(int index);  // P1 red, P2 blue, P3 green, P4 yellow, as the MULTI join screen

// Right after Begin: the window's wooden panel, drawn under what follows.
void panel();

// The page's heading, large and gold ("ONLINE"); a plain line until the game's font is ready.
void heading(const std::string& text);

// A line of button prompts: the picture of each control (the pad's, or the keyboard's keys), then
// what it does. `controls` names them: "a" (select), "b" (back).
void prompts(const std::vector<std::pair<std::string, std::string>>& controls);

} // namespace conker::lobby_style
