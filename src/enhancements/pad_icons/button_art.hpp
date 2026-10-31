// The button pictures, drawn when they are first wanted: no picture is stored in this repository.
#pragma once

#include <string>
#include <vector>

#include "enhancements/pad_icons/pad_icons.hpp"

namespace conker::pad_icons {

struct Picture {
    std::string name;            // "a", "a_lit", "z", "c_left_small", ...
    int width, height;           // pixels: 4 for each game texel
    std::vector<uint8_t> rgba;   // straight alpha, rows top first
};

// Every picture of `style`'s set (N64, Xbox, PlayStation or the keyboard's), drawn on every core. With
// `camera_swapped`, C-left is the right stick pushed right (input::camera_swapped), else left.
std::vector<Picture> draw_set(Style style, bool camera_swapped);

// Keycaps as the keyboard set draws them, one a label ("ESC", "ENTER"; longer than three letters
// makes a wide key), each named by its label.
std::vector<Picture> draw_keys(const std::vector<std::string>& labels);

} // namespace conker::pad_icons
