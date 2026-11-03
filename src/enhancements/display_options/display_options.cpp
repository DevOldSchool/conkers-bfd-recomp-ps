// A VOLUME row and a CONTROLS row under the sound row on the title menu's OPTIONS page, and display rows between
// it and CHEATS: whether the sharper text and menus are drawn (SHARP TEXT or ORIGINAL
// TEXT), the display mode (WINDOWED, BORDERLESS, FULL SCREEN), the picture's shape (4:3 or
// WIDESCREEN), the resolution the picture is drawn at in that mode, and whether it is scaled to
// fill the window or screen or drawn at its own size.
// Native helpers called from the [[patches.hook]] snippets in config/us.toml (func_151DF1BC,
// func_151E3344); CONKER_DISPLAY_OPTIONS=0 leaves the page as it was.
//
// func_151DF1BC draws the OPTIONS page with func_151E3344, the menu engine Rare also uses for the
// multiplayer set-up. Its first argument is a mask of the options to show, one bit for each of
// the 23 settings in D_800E0BE0 (0x200200 here: 9 sound, 21 CHEATS), and it lays out, draws and
// steers through those rows itself, in bit order. Option n's limits are D_8008FE6C[n] and
// D_8008FE84[n]; how it is drawn comes from its record in D_800AB64C: a label, then for a text
// option the index of each value's string in the language text table (D_800E0BD8). Values wrap
// around at either end.
//
// Each row borrows an option with text values that the page does not show, for the length of the
// call: its value, limits and label are swapped for the row's, and its strings for the row's
// words. Everything is put back when the call returns, so the multiplayer settings and the
// options saved to EEPROM never see these settings, which are kept in
// conker-recomp-data/display.cfg and audio.cfg instead (src/platform/display_mode.cpp,
// src/platform/audio.cpp).
//
// * The volume row borrows option 0 (three values) as a carousel: the volume is in the middle,
//   and a step left or right turns it down or up by 10%, stopping at OFF and 100%. Every text
//   option after the sound row's (9) is taken by a display row, so the engine puts this row
//   first; it is drawn under the sound row, and moving up or down follows the rows down the page. The volume scales the host's audio output only
//   (src/platform/audio.cpp), so it is the same in every game mode, multiplayer included.
// * The controls row borrows option 8 (PICTURE 4:3/16:9, two values, hidden on the US page and
//   read by nothing else; see the shape row), drawn under the volume row. A step either way, or A
//   on it, opens the controls page over this one (src/enhancements/debug_menu/debug_menu.hpp):
//   each controller's buttons and keys, the aim's inversion and MOUSE AIM. Not online, where the
//   row is left out.
// * The sharp row borrows option 10 (five values) as a carousel like the resolution row: the
//   setting is in the middle and the other one on either side, so a step either way switches it.
//   SHARP TEXT loads the built-in sharper font and menus (src/platform/renderer_rt64.cpp),
//   ORIGINAL TEXT drops them. The option keeps its five values because func_151E3344 finds each
//   option's record by adding up the records before it, whose lengths follow their values.
// * The mode row borrows option 12 (GOAL: three values), one value per mode.
// * The shape row borrows option 15 (two values): 4:3, or WIDESCREEN, where RT64 widens the view
//   to the window's or screen's shape, kept for each mode like the fill. The game has a PICTURE 4:3/16:9 setting of its own (option
//   8), hidden on the US OPTIONS page, but nothing in the US game reads it.
// * The resolution row borrows option 16 (AI LEVEL: five values) as a carousel, because how many
//   sizes there are depends on the display: before each call its value is put in the middle with
//   the neighbouring sizes on either side, and a step left or right afterwards moves along the
//   list. A window offers the multiples of the 292x216 picture (384x216 in widescreen) that fit
//   the display; covering the screen it offers the multiples up to the first that covers the
//   screen's height, as wide as the screen in widescreen.
// * The fill row borrows option 19 (DIRECTION: two values): FILL SCREEN or CENTERED, kept for each
//   mode (src/platform/display_mode.hpp).
//
// The two joysticks that flank the selected row are moved apart for these rows, since their
// longer words would run under them. The engine puts every row 34 pixels below the last; with
// these rows the page would reach the bottom of the picture, so the display rows are grouped
// closer together, with more room before CHEATS, and the joysticks follow the same rows.
#include <cstdint>
#include <iterator>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "recomp.h"
#include "platform/audio.hpp"
#include "platform/display_mode.hpp"
#include "platform/input.hpp"
#include "enhancements/debug_menu/debug_menu.hpp"
#include "enhancements/status.hpp"
#include "runtime/frame_input.hpp"

namespace {

using conker::display::Mode;

inline gpr kseg0(uint32_t address) { return static_cast<gpr>(static_cast<int32_t>(address)); }

constexpr uint32_t kOptionsPageMask = 0x200200;  // func_151DF1BC's options: sound and CHEATS
constexpr uint32_t kValues = 0x800E0BE0;         // D_800E0BE0[23]: the settings
constexpr uint32_t kMinimums = 0x8008FE6C;       // D_8008FE6C[23]
constexpr uint32_t kMaximums = 0x8008FE84;       // D_8008FE84[23]
constexpr uint32_t kRecords = 0x800AB64C;        // D_800AB64C: each option's record
constexpr uint32_t kTextTable = 0x800E0BD8;      // D_800E0BD8: the language's string pointers
constexpr uint32_t kSelected = 0x8008FE34;       // D_8008FE34: the selected option
constexpr int8_t kNoLabel = -1;
constexpr int kStickSpread = 18;                 // extra room on each side of the rows, in pixels
constexpr int kEngineRowSpacing = 0x22;          // func_151E3344's row pitch on this page
constexpr int kPageTop = 0x36;                   // the top row's y: 0x46 in the engine, moved up for the volume and mouse rows
constexpr int kRowYSlot = 0x138;                 // func_151E3344's stack: the row's y (low half)
constexpr uint32_t kStickFrom = 0x8008FDE4;      // D_8008FDE4: the row the joysticks move from
constexpr uint32_t kStickProgress = 0x8008FDE8;  // D_8008FDE8: how far they have moved (float)
constexpr int kStickToSlot = 0x108;              // func_151E3344's stack: the row they move to
constexpr int kMaxRows = 23;
constexpr uint32_t kStringArea = 0x807FA000;     // free RDRAM, see pause_save.cpp
constexpr uint32_t kStringStride = 0x20;
constexpr int kMaxValues = 5;

// A multiplayer option lent to a row. Its record is the label, then one string index per value.
struct Borrowed {
    int option;
    uint32_t record_offset;  // into D_800AB64C
    int value_count;
    int8_t record[1 + kMaxValues];  // as the game has it; checked before borrowing
    uint32_t strings;               // where the row's words go
    // The string indices the row's values use instead of the record's own, when another row
    // repoints one of those (option 0's BORDERLESS is also option 12's); none if empty.
    int8_t indices[kMaxValues];

    // Saved during the call.
    int8_t saved_value, saved_minimum, saved_maximum;
    int8_t saved_record[1 + kMaxValues];
    int32_t saved_pointers[kMaxValues];
};

Borrowed g_mode_row{12, 31, 3, {2, 28, 4, 29}, kStringArea};
Borrowed g_scale_row{16, 42, 5, {-6, 35, 36, 37, 38, 39}, kStringArea + 3 * kStringStride};
Borrowed g_fill_row{19, 52, 2, {-62, 43, 44}, kStringArea + 8 * kStringStride};
Borrowed g_shape_row{15, 39, 2, {31, 32, 33}, kStringArea + 10 * kStringStride};
Borrowed g_sharp_row{10, 23, 5, {21, 22, 25, 23, 24, 26}, kStringArea + 12 * kStringStride};
// Option 0 shares string 4 with option 12; string 2 is both options' label, unused while borrowed.
Borrowed g_volume_row{0, 0, 3, {2, 3, 4, 5}, kStringArea + 17 * kStringStride, {3, 2, 5}};
Borrowed g_mouse_row{8, 18, 2, {13, 14, 15}, kStringArea + 20 * kStringStride};  // CONTROLS
bool g_controls_shown = false;   // this call has the controls row
bool g_a_was_held = false;       // controller 1's A at the last call, for a press on that row
constexpr int8_t kCarouselMiddle = 2;
constexpr int8_t kVolumeMiddle = 1;

bool g_active = false;
int g_row_option[kMaxRows];   // each shown row's option, in the engine's (option) order
int g_row_count = 0;
int g_drawn_option[kMaxRows];  // the shown options in the order they are drawn down the page
int g_option_y[kMaxRows];      // where each shown option's row is drawn
int g_selected_before = 0;     // the selected option when the call began

// The gap below a row, by the option it shows.
int gap_after(int option) {
    switch (option) {
        case 9: return 20;    // the sound icon, then the volume
        case 0:               // the volume, the mouse speed, then the display rows
        case 8:
        case 10:
        case 12:
        case 15:
        case 16: return 15;   // between the display rows
        case 19: return 24;   // the display rows, then CHEATS
        default: return kEngineRowSpacing;
    }
}

// The engine places the rows in option order, which puts the volume row (option 0) and the mouse
// speed row (8) first; they are drawn under the sound row instead, and the rest keep their order.
void lay_out(uint32_t mask) {
    g_row_count = 0;
    for (int option = 0; option < kMaxRows; option++) {
        if (mask & (1u << option)) {
            g_row_option[g_row_count++] = option;
        }
    }
    const int volume = g_volume_row.option;
    const bool has_volume = (mask & (1u << volume)) != 0;
    const int mouse = g_mouse_row.option;
    const bool has_mouse = (mask & (1u << mouse)) != 0;
    int drawn = 0;
    int y = kPageTop;
    auto place = [&](int option) {
        g_drawn_option[drawn++] = option;
        g_option_y[option] = y;
        y += gap_after(option);
    };
    for (int row = 0; row < g_row_count; row++) {
        const int option = g_row_option[row];
        if (option == volume || option == mouse) {
            continue;
        }
        place(option);
        if (option == 9 && has_volume) {
            place(volume);
        }
        if (option == 9 && has_mouse) {
            place(mouse);
        }
    }
}

int row_y(int row) {
    return g_option_y[g_row_option[row < 0 ? 0 : row >= g_row_count ? g_row_count - 1 : row]];
}

int row_of(const int* options, int option) {
    for (int row = 0; row < g_row_count; row++) {
        if (options[row] == option) {
            return row;
        }
    }
    return -1;
}

// The engine moves the selection a row up or down in its own order; move it the same way down
// the page instead.
int follow_page_order(int before, int after) {
    const int from = row_of(g_row_option, before);
    const int to = row_of(g_row_option, after);
    const int drawn = row_of(g_drawn_option, before);
    if (before == after || from < 0 || to < 0 || drawn < 0) {
        return after;
    }
    const int count = g_row_count;
    if (to == (from + 1) % count) {
        return g_drawn_option[(drawn + 1) % count];
    }
    if (to == (from + count - 1) % count) {
        return g_drawn_option[(drawn + count - 1) % count];
    }
    return after;
}
int8_t g_shown_mode = 0;
int8_t g_shown_shape = 0;
int8_t g_shown_fill = 0;
std::vector<int> g_scales;  // the resolution row's choices in this mode
int g_scale_index = 0;      // the one shown

bool display_options_enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("CONKER_DISPLAY_OPTIONS");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}

uint32_t record_address(const Borrowed& row) { return kRecords + row.record_offset; }

int string_index(const Borrowed& row, int value) {
    return row.indices[0] != 0 ? row.indices[value] : row.record[1 + value];
}

uint32_t string_slot(uint8_t* rdram, int index) {
    return static_cast<uint32_t>(MEM_W(0, kseg0(kTextTable))) + static_cast<uint32_t>(index) * 4;
}

bool record_matches(uint8_t* rdram, const Borrowed& row) {
    for (int i = 0; i <= row.value_count; i++) {
        if (MEM_B(i, kseg0(record_address(row))) != row.record[i]) {
            return false;
        }
    }
    return true;
}

void write_string(uint8_t* rdram, uint32_t address, const char* text) {
    for (uint32_t c = 0;; c++) {
        MEM_B(c, kseg0(address)) = text[c];
        if (text[c] == '\0') {
            break;
        }
    }
}

// Lends the option to the row: value, limits, no label, and the given words for its values.
void borrow(uint8_t* rdram, Borrowed& row, int8_t value, const std::vector<std::string>& words) {
    row.saved_value = MEM_B(row.option, kseg0(kValues));
    row.saved_minimum = MEM_B(row.option, kseg0(kMinimums));
    row.saved_maximum = MEM_B(row.option, kseg0(kMaximums));
    for (int i = 0; i <= row.value_count; i++) {
        row.saved_record[i] = MEM_B(i, kseg0(record_address(row)));
    }
    MEM_B(row.option, kseg0(kValues)) = value;
    MEM_B(row.option, kseg0(kMinimums)) = 0;
    MEM_B(row.option, kseg0(kMaximums)) = static_cast<int8_t>(row.value_count - 1);
    MEM_B(0, kseg0(record_address(row))) = kNoLabel;
    for (int i = 0; i < row.value_count; i++) {
        // The string indices are repointed, so the record keeps its length.
        const int index = string_index(row, i);
        MEM_B(1 + i, kseg0(record_address(row))) = static_cast<int8_t>(index);
        const uint32_t slot = string_slot(rdram, index);
        row.saved_pointers[i] = MEM_W(0, kseg0(slot));
        const uint32_t text = row.strings + static_cast<uint32_t>(i) * kStringStride;
        write_string(rdram, text, words[static_cast<size_t>(i)].c_str());
        MEM_W(0, kseg0(slot)) = static_cast<int32_t>(kseg0(text));
    }
}

// Gives the option back and returns the value the row was left at.
int8_t give_back(uint8_t* rdram, Borrowed& row) {
    const int8_t value = MEM_B(row.option, kseg0(kValues));
    MEM_B(row.option, kseg0(kValues)) = row.saved_value;
    MEM_B(row.option, kseg0(kMinimums)) = row.saved_minimum;
    MEM_B(row.option, kseg0(kMaximums)) = row.saved_maximum;
    for (int i = 0; i <= row.value_count; i++) {
        MEM_B(i, kseg0(record_address(row))) = row.saved_record[i];
    }
    for (int i = 0; i < row.value_count; i++) {
        MEM_W(0, kseg0(string_slot(rdram, string_index(row, i)))) = row.saved_pointers[i];
    }
    return value;
}

std::string volume_words(int volume) {
    volume = volume < 0 ? 0 : volume > conker::audio::kMaxVolume ? conker::audio::kMaxVolume : volume;
    if (volume == 0) {
        return "VOLUME OFF";
    }
    char text[32];
    std::snprintf(text, sizeof(text), "VOLUME %d%%", volume * 10);
    return text;
}

std::string scale_words(Mode mode, int scale) {
    const int width = mode == Mode::Windowed ? conker::display::window_picture_width(scale)
                                             : conker::display::screen_picture_width(scale);
    char text[32];
    std::snprintf(text, sizeof(text), "%d X %d", width, conker::display::kPictureHeight * scale);
    return text;
}

// The resolution row's choices for the mode, and which one is in use.
void list_scales(Mode mode) {
    g_scales.clear();
    int current;
    if (mode == Mode::Windowed) {
        for (int scale = conker::display::kMinScale; scale <= conker::display::window_max(); scale++) {
            g_scales.push_back(scale);
        }
        current = conker::display::window_scale();
    } else {
        for (int scale = conker::display::kMinScale; scale <= conker::display::screen_max(); scale++) {
            g_scales.push_back(scale);
        }
        current = conker::display::screen_scale();
    }
    g_scale_index = 0;
    for (size_t i = 0; i < g_scales.size(); i++) {
        if (g_scales[i] == current) {
            g_scale_index = static_cast<int>(i);
        }
    }
}

int wrap(int index) {
    const int count = static_cast<int>(g_scales.size());
    return ((index % count) + count) % count;
}

}  // namespace

// Before func_151DF1BC's `jal func_151E3344` ($a0 = the options to show).
extern "C" void conker_display_options_begin(uint8_t* rdram, recomp_context* ctx) {
    if (!display_options_enabled() || static_cast<uint32_t>(ctx->r4) != kOptionsPageMask ||
        !record_matches(rdram, g_mode_row) || !record_matches(rdram, g_scale_row) ||
        !record_matches(rdram, g_fill_row) || !record_matches(rdram, g_shape_row) ||
        !record_matches(rdram, g_sharp_row) || !record_matches(rdram, g_volume_row) ||
        !record_matches(rdram, g_mouse_row)) {
        return;
    }
    const int volume = conker::audio::volume();
    borrow(rdram, g_volume_row, kVolumeMiddle, {volume_words(volume - 1), volume_words(volume), volume_words(volume + 1)});
    ctx->r4 |= 1u << g_volume_row.option;

    g_controls_shown = conker::debug_menu::controls_available();
    if (g_controls_shown) {
        borrow(rdram, g_mouse_row, 0, {"CONTROLS", "CONTROLS"});
        ctx->r4 |= 1u << g_mouse_row.option;
    }

    const bool sharp = conker::display::sharp();
    const char* const same = sharp ? "SHARP TEXT" : "ORIGINAL TEXT";
    const char* const other = sharp ? "ORIGINAL TEXT" : "SHARP TEXT";
    borrow(rdram, g_sharp_row, kCarouselMiddle, {same, other, same, other, same});
    ctx->r4 |= 1u << g_sharp_row.option;

    const Mode mode = conker::display::current();
    g_shown_mode = static_cast<int8_t>(mode);
    borrow(rdram, g_mode_row, g_shown_mode, {"WINDOWED", "BORDERLESS", "FULL SCREEN"});

    g_shown_shape = conker::display::widescreen(mode) ? 1 : 0;
    borrow(rdram, g_shape_row, g_shown_shape, {"4:3", "WIDESCREEN"});

    list_scales(mode);
    std::vector<std::string> words;
    for (int i = -kCarouselMiddle; i < g_scale_row.value_count - kCarouselMiddle; i++) {
        words.push_back(scale_words(mode, g_scales[static_cast<size_t>(wrap(g_scale_index + i))]));
    }
    borrow(rdram, g_scale_row, kCarouselMiddle, words);

    ctx->r4 |= (1u << g_mode_row.option) | (1u << g_shape_row.option) | (1u << g_scale_row.option);

    g_shown_fill = conker::display::fill(mode) ? 0 : 1;
    borrow(rdram, g_fill_row, g_shown_fill, {"FILL SCREEN", "CENTERED"});
    ctx->r4 |= 1u << g_fill_row.option;
    lay_out(static_cast<uint32_t>(ctx->r4));
    g_selected_before = MEM_BU(0, kseg0(kSelected));
    g_active = true;
}

// After that call: take the rows' values and give the options back.
extern "C" void conker_display_options_end(uint8_t* rdram, recomp_context* /*ctx*/) {
    if (!g_active) {
        return;
    }
    g_active = false;
    MEM_B(0, kseg0(kSelected)) =
        static_cast<int8_t>(follow_page_order(g_selected_before, MEM_BU(0, kseg0(kSelected))));
    const int8_t volume_step = static_cast<int8_t>(give_back(rdram, g_volume_row) - kVolumeMiddle);
    const bool controls_stepped = g_controls_shown && give_back(rdram, g_mouse_row) != 0;
    uint16_t buttons;
    float x;
    float y;
    conker::frame_input::read(&buttons, &x, &y, 0);
    const bool a_pressed = (buttons & conker::input::kA) != 0 && !g_a_was_held;
    g_a_was_held = (buttons & conker::input::kA) != 0;
    const int8_t mode = give_back(rdram, g_mode_row);
    const int8_t shape = give_back(rdram, g_shape_row);
    const int8_t step = static_cast<int8_t>(give_back(rdram, g_scale_row) - kCarouselMiddle);
    const int8_t fill = give_back(rdram, g_fill_row);
    const int8_t sharp_step = static_cast<int8_t>(give_back(rdram, g_sharp_row) - kCarouselMiddle);
    if (volume_step != 0) {
        conker::audio::set_volume(conker::audio::volume() + (volume_step > 0 ? 1 : -1));
    } else if (controls_stepped || (g_controls_shown && a_pressed && MEM_BU(0, kseg0(kSelected)) == g_mouse_row.option)) {
        conker::debug_menu::open_controls();
    } else if (sharp_step != 0) {
        conker::display::request_sharp(!conker::display::sharp());
    } else if (mode != g_shown_mode && mode >= 0 && mode < conker::display::kModeCount) {
        conker::display::request(static_cast<Mode>(mode));
    } else if (shape != g_shown_shape) {
        conker::display::request_widescreen(static_cast<Mode>(g_shown_mode), shape == 1);
    } else if (step != 0 && g_scales.size() > 1) {
        const int scale = g_scales[static_cast<size_t>(wrap(g_scale_index + (step > 0 ? 1 : -1)))];
        if (static_cast<Mode>(g_shown_mode) == Mode::Windowed) {
            conker::display::request_window_scale(scale);
        } else {
            conker::display::request_screen_scale(scale);
        }
    } else if (fill != g_shown_fill) {
        conker::display::request_fill(static_cast<Mode>(g_shown_mode), fill == 0);
    }
}

// In func_151E3344 before it draws the joysticks at 0x94 +- $s0: further apart for these rows.
extern "C" void conker_display_options_sticks(uint8_t* rdram, recomp_context* ctx) {
    const int selected = MEM_BU(0, kseg0(kSelected));
    if (g_active && (selected == g_mode_row.option || selected == g_shape_row.option ||
                     selected == g_scale_row.option || selected == g_fill_row.option ||
                     selected == g_sharp_row.option || selected == g_volume_row.option ||
                     selected == g_mouse_row.option)) {
        ctx->r16 += kStickSpread;
    }
}

// In func_151E3344 before `addiu $s2, $s2, 0x22`, which moves $s2 to the next row's y ($s0 = the
// option just placed). The row is drawn at the y just stored from $s2, which is replaced by the
// row's place on this page.
extern "C" void conker_display_options_row_y(uint8_t* rdram, recomp_context* ctx) {
    const int option = static_cast<int>(ctx->r16);
    if (g_active && option >= 0 && option < kMaxRows) {
        MEM_W(kRowYSlot, ctx->r29) = g_option_y[option];
    }
}

// In func_151E3344 once it has placed the joysticks ($s1 = 0x46 + 0x22 * the row they are moving
// through, from D_8008FDE4 towards the selected row): the same rows' y.
extern "C" void conker_display_options_stick_y(uint8_t* rdram, recomp_context* ctx) {
    if (!g_active) {
        return;
    }
    const int from = MEM_B(0, kseg0(kStickFrom));
    const int to = MEM_W(kStickToSlot, ctx->r29);
    const uint32_t bits = static_cast<uint32_t>(MEM_W(0, kseg0(kStickProgress)));
    float progress;
    std::memcpy(&progress, &bits, sizeof(progress));
    const float y = static_cast<float>(row_y(from)) + progress * static_cast<float>(row_y(to) - row_y(from));
    ctx->r17 = static_cast<int32_t>(y);
}

nlohmann::json conker::status::display_options() {
    return enhancement("display_options", "Display options", display_options_enabled(), "CONKER_DISPLAY_OPTIONS");
}
