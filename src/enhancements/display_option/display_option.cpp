// A display mode row (WINDOWED, BORDERLESS, FULL SCREEN) on the title menu's OPTIONS page, between
// the sound row and CHEATS. Native helpers called from the [[patches.hook]] snippets in
// config/us.toml (func_151DF1BC); CONKER_DISPLAY_OPTION=0 leaves the page as it was.
//
// func_151DF1BC draws the OPTIONS page with func_151E3344, the menu engine Rare also uses for the
// multiplayer set-up. Its first argument is a mask of the options to show, one bit for each of
// the 23 settings in D_800E0BE0 (0x200200 here: 9 sound, 21 CHEATS), and it lays out, draws and
// steers through those rows itself. Option n's limits are D_8008FE6C[n] and D_8008FE84[n]; how it
// is drawn comes from its record in D_800AB64C: a label, then for a text option the index of each
// value's string in the language text table (D_800E0BD8).
//
// The row borrows option 12, a multiplayer GOAL setting with three text values, for the length of
// the call: its value, limits and label are swapped for the display mode's, and its three strings
// for WINDOWED, BORDERLESS and FULL SCREEN. Everything is put back when the call returns, so the
// multiplayer setting and the options saved to EEPROM never see the display mode, which is kept in
// conker-recomp-data/display.cfg instead (src/platform/display_mode.cpp). The row's values are the
// modes' (src/platform/display_mode.hpp); like the game's own rows it wraps around at either end.
// The two joysticks that flank the selected row are moved apart for it, since its longer words
// would run under them.
#include <cstdint>
#include <cstdlib>

#include "recomp.h"
#include "platform/display_mode.hpp"

namespace {

inline gpr kseg0(uint32_t address) { return static_cast<gpr>(static_cast<int32_t>(address)); }

constexpr uint32_t kOptionsPageMask = 0x200200;  // func_151DF1BC's options: sound and CHEATS
constexpr int kRow = 12;                         // the option borrowed for the row

constexpr uint32_t kValues = 0x800E0BE0;        // D_800E0BE0[23]: the settings
constexpr uint32_t kMinimums = 0x8008FE6C;      // D_8008FE6C[23]
constexpr uint32_t kMaximums = 0x8008FE84;      // D_8008FE84[23]
constexpr uint32_t kTextTable = 0x800E0BD8;     // D_800E0BD8: the language's string pointers
// Option 12's record in D_800AB64C: label GOAL (2), then BOMB BATTLE (28), BATTLE (4), CIVILIAN (29).
constexpr uint32_t kRecord = 0x800AB64C + 31;
constexpr int8_t kRecordExpected[4] = {2, 28, 4, 29};
constexpr int8_t kNoLabel = -1;
constexpr int8_t kStrings[conker::display::kModeCount] = {28, 29, 30};  // repointed during the call

constexpr uint32_t kStringArea = 0x807FA000;  // free RDRAM, see pause_save.cpp
constexpr uint32_t kStringStride = 0x20;
const char* const kLabels[conker::display::kModeCount] = {"WINDOWED", "BORDERLESS", "FULL SCREEN"};
constexpr uint32_t kSelected = 0x8008FE34;    // D_8008FE34: the selected option
constexpr int kStickSpread = 18;              // extra room on each side of the row, in pixels


struct Saved {
    int8_t value, minimum, maximum;
    int8_t record[4];
    int32_t strings[conker::display::kModeCount];
};

Saved g_saved;
bool g_active = false;
int8_t g_shown = 0;

bool display_option_enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("CONKER_DISPLAY_OPTION");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}

bool record_matches(uint8_t* rdram) {
    for (int i = 0; i < 4; i++) {
        if (MEM_B(i, kseg0(kRecord)) != kRecordExpected[i]) {
            return false;
        }
    }
    return true;
}

uint32_t string_slot(uint8_t* rdram, int index) {
    return static_cast<uint32_t>(MEM_W(0, kseg0(kTextTable))) + static_cast<uint32_t>(index) * 4;
}

}  // namespace

// Before func_151DF1BC's `jal func_151E3344` ($a0 = the options to show).
extern "C" void conker_display_option_begin(uint8_t* rdram, recomp_context* ctx) {
    if (!display_option_enabled() || static_cast<uint32_t>(ctx->r4) != kOptionsPageMask || !record_matches(rdram)) {
        return;
    }
    g_saved.value = MEM_B(kRow, kseg0(kValues));
    g_saved.minimum = MEM_B(kRow, kseg0(kMinimums));
    g_saved.maximum = MEM_B(kRow, kseg0(kMaximums));
    for (int i = 0; i < 4; i++) {
        g_saved.record[i] = MEM_B(i, kseg0(kRecord));
    }
    for (int i = 0; i < conker::display::kModeCount; i++) {
        g_saved.strings[i] = MEM_W(0, kseg0(string_slot(rdram, kStrings[i])));
    }

    g_shown = static_cast<int8_t>(conker::display::current());
    MEM_B(kRow, kseg0(kValues)) = g_shown;
    MEM_B(kRow, kseg0(kMinimums)) = 0;
    MEM_B(kRow, kseg0(kMaximums)) = conker::display::kModeCount - 1;
    MEM_B(0, kseg0(kRecord)) = kNoLabel;
    for (int i = 0; i < conker::display::kModeCount; i++) {
        MEM_B(1 + i, kseg0(kRecord)) = kStrings[i];
        const uint32_t text = kStringArea + static_cast<uint32_t>(i) * kStringStride;
        for (uint32_t c = 0;; c++) {
            MEM_B(c, kseg0(text)) = kLabels[i][c];
            if (kLabels[i][c] == '\0') {
                break;
            }
        }
        MEM_W(0, kseg0(string_slot(rdram, kStrings[i]))) = static_cast<int32_t>(kseg0(text));
    }
    ctx->r4 |= 1u << kRow;
    g_active = true;
}

// After that call: take the row's value and put option 12 back.
extern "C" void conker_display_option_end(uint8_t* rdram, recomp_context* /*ctx*/) {
    if (!g_active) {
        return;
    }
    g_active = false;
    const int8_t chosen = MEM_B(kRow, kseg0(kValues));
    MEM_B(kRow, kseg0(kValues)) = g_saved.value;
    MEM_B(kRow, kseg0(kMinimums)) = g_saved.minimum;
    MEM_B(kRow, kseg0(kMaximums)) = g_saved.maximum;
    for (int i = 0; i < 4; i++) {
        MEM_B(i, kseg0(kRecord)) = g_saved.record[i];
    }
    for (int i = 0; i < conker::display::kModeCount; i++) {
        MEM_W(0, kseg0(string_slot(rdram, kStrings[i]))) = g_saved.strings[i];
    }
    if (chosen != g_shown && chosen >= 0 && chosen < conker::display::kModeCount) {
        conker::display::request(static_cast<conker::display::Mode>(chosen));
    }
}

// In func_151E3344 before it draws the joysticks at 0x94 +- $s0: further apart for the row.
extern "C" void conker_display_option_sticks(uint8_t* rdram, recomp_context* ctx) {
    if (g_active && MEM_BU(0, kseg0(kSelected)) == kRow) {
        ctx->r16 += kStickSpread;
    }
}
