// SAVE and LOAD in the pause menu. Native helpers called from the [[patches.hook]] snippets in
// config/us.toml (func_151DDC20, func_151ED430, func_15043384).
//
// func_151DDC20 is the single-player pause menu: a lever between QUIT on the left and CONT... on
// the right. When replaying a chapter it has a third option, RESTART, above QUIT. The game saves
// only at checkpoints and when quitting (QUIT, then Y, queues func_15007718's save before it
// leaves). In normal play the recomp lifts the lever row and puts two signs under it:
//
//     QUIT  lever  CONT...
//     LOAD         SAVE
//
// Left and right move along a row, down and up between rows, and the stick's down-left and
// down-right diagonals pick LOAD and SAVE directly. The menu's own logic runs in its
// three-option form with SAVE in RESTART's place (selection 1). LOAD has no selection of its
// own: it is selection 2 (QUIT) with g_load_selected set, and QUIT is drawn unlit meanwhile.
//
// A on SAVE queues the same save as quitting, without leaving, and
// src/enhancements/pause_save/resume_point.cpp records where Conker is so the save starts there.
// "SAVED" then shows under the lever and fades out. A on LOAD reloads the save and starts it where
// it was made (src/enhancements/pause_save/resume_point.cpp).
//
// The signs are drawn with sprite selectors that never appear in normal play: SAVE with
// RESTART's (0x21 unlit, 0x22 lit), LOAD with the title menu's PLAY (0x16, 0x17). While they are
// on screen, the tile lookup in func_151ED430 hands out our own tiles for those selectors'
// resources. They are built from the CONT... and QUIT signs' tiles as the game loads them: the
// lettering inside the frame is cleared and src/enhancements/pause_save/pause_sign_letters.inc
// (tools/make_pause_signs.py) is drawn instead, so no ROM art is stored in the repository.
#include <chrono>
#include <cstdint>
#include <cstdlib>

#include "recomp.h"
#include "enhancements/pause_save/resume_point.hpp"
#include "enhancements/status.hpp"

extern "C" void func_15042D94(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_1510D0EC(uint8_t* rdram, recomp_context* ctx);

namespace {

inline gpr kseg0(uint32_t address) { return static_cast<gpr>(static_cast<int32_t>(address)); }

constexpr uint32_t kChapterReplay = 0x800BE616;   // D_800BE616: playing from CHAPTERS, saves off
constexpr uint32_t kSaveSlot = 0x8008FDA8;        // D_8008FDA8: the game slot in play, -1 for none
constexpr uint32_t kEepromAction = 0x80082BB4;    // D_80082BB4: request for func_1501C57C
constexpr uint32_t kEepromSlot = 0x800BE3EC;      // D_800BE3EC: its slot argument
constexpr uint32_t kPauseSelection = 0x8008FE2C;  // D_8008FE2C: 0 CONT, 1 RESTART, 2 QUIT
constexpr uint32_t kPauseClosing = 0x8008FEEC;    // D_8008FEEC/F0: set to 10 and 5 to close it,
constexpr uint32_t kPauseClose = 0x8008FEF0;      // as A or B on CONT... does
constexpr uint32_t kPreviousStickX = 0x8008FEF4;  // D_8008FEF4: last frame's stick, for edges
constexpr uint32_t kPreviousStickY = 0x800E0A85;  // D_800E0A85
constexpr uint32_t kTextColor = 0x800CBD60;       // D_800CBD60: RGBA given to the next text record
constexpr uint32_t kTextX = 0x800CBD70;           // D_800CBD70/72: where it goes (func_150432FC)
constexpr uint32_t kTextY = 0x800CBD72;

// The game never reads osMemSize and keeps to the low 4 MiB, so the top of the 8 MiB the runtime
// provides is free. RT64 reads textures from RDRAM, so the tiles must live below 8 MiB.
constexpr uint32_t kSignTiles = 0x807F0000;      // 32x32 RGBA32 tiles, 4 per sign: unlit L/R, lit L/R
constexpr uint32_t kSavedText = 0x807F8000;      // the "SAVED" format string
constexpr uint32_t kSpriteFormat = 0x807F8010;   // "#": draw the sprite selector given

// func_151DDC20's stack frame
constexpr int kOutgoingArg = 0x10;               // first stack argument of its calls
constexpr int kSoundSlot = 0x84;                 // sound to play this frame
constexpr int kOptionCountSlot = 0x88;           // option count, 2 or 3
constexpr int kLayoutYSlot = 0x8C;               // layout offset: 5 for two options
constexpr int kSpecialPauseSlot = 0x94;          // flag for the CONT-only pause

// Options, as the stick moves between them. The game's selection is the same except for LOAD.
enum Option : int8_t { kCont = 0, kSave = 1, kQuit = 2, kLoad = 3 };

constexpr int8_t kEepromSave = 2;
constexpr int kStickEdge = 30;
constexpr int kLift = 24;                        // how far the lever row moves up
constexpr int kLeftSignX = 0x53;                 // QUIT is drawn at (0x53, layout + 0x73)
constexpr int kRightSignX = 0xD5;                // CONT... at (0xD5, layout + 0x73)
constexpr int kSignY = 0x73;
constexpr int kLowerSignY = kSignY + 32;         // SAVE and LOAD go right below them
constexpr int kLeverX = 0x94;                    // the lever is drawn at (0x94, layout + 0x73)
constexpr int kSavedTextY = kSignY + 24;         // "SAVED" goes below it
constexpr int kLoadSelector = 0x16;              // PLAY unlit; 0x17 lit
constexpr int32_t kLeverSound = 0x62D;
constexpr int32_t kSelectSound = 0x500;
constexpr uint32_t kNotLoaded = 0x80000000;      // func_1510D0EC: tile not resident yet

constexpr double kSavedHoldSeconds = 2.0;
constexpr double kSavedFadeSeconds = 1.0;

using Clock = std::chrono::steady_clock;

const uint8_t kLetters[2][2][32][64][4] = {  // [sign][unlit, lit]
#include "enhancements/pause_save/pause_sign_letters.inc"
};

bool pause_save_enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("CONKER_PAUSE_SAVE");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}

// True while func_151DDC20 shows SAVE and LOAD (set up by conker_pause_save_layout).
bool signs_shown(uint8_t* rdram, recomp_context* ctx) {
    return pause_save_enabled() && MEM_W(kOptionCountSlot, ctx->r29) == 3 &&
           MEM_BU(0, kseg0(kChapterReplay)) == 0;
}

bool g_load_selected = false;

Option current_option(uint8_t* rdram) {
    const int8_t selection = MEM_B(0, kseg0(kPauseSelection));
    if (selection == kQuit && g_load_selected) {
        return kLoad;
    }
    g_load_selected = false;
    return static_cast<Option>(selection);
}

void select_option(uint8_t* rdram, Option option) {
    MEM_B(0, kseg0(kPauseSelection)) = option == kLoad ? kQuit : option;
    g_load_selected = option == kLoad;
}

// --- the signs -----------------------------------------------------------------------------------

struct SignKind {
    int source_tiles;  // the game sign whose frame it uses: unlit tiles, then lit
    int target_tiles;  // the selector's resources it replaces
};

constexpr SignKind kSigns[2] = {
    {2003, 2062},  // SAVE: CONT...'s frame, drawn as RESTART
    {2044, 2039},  // LOAD: QUIT's frame, drawn as PLAY
};

struct SignFrame {
    uint8_t pixels[32][64][4];  // linear RGBA
    unsigned captured = 0;      // bit per half
    bool ready = false;
};

SignFrame g_frames[2][2];    // [sign][unlit, lit]
bool g_signs_drawn = false;  // the signs are in this frame's sprite list

// RGBA32 tiles are loaded with LoadBlock and no line swap, so odd rows are stored with the two
// 8-byte halves of each 16-byte group exchanged, as TMEM holds them.
uint32_t tile_offset(int x, int y) { return static_cast<uint32_t>((y * 32 + ((y & 1) ? x ^ 2 : x)) * 4); }

uint32_t sign_tile(int sign, int variant, int half) {
    return kSignTiles + static_cast<uint32_t>(sign * 4 + variant * 2 + half) * 0x1000;
}

// Everything more than 4 pixels inside the frame's opaque outline is the lettering.
void find_inside(const SignFrame& frame, bool inside[32][64]) {
    bool outside[32][64] = {};
    int stack[32 * 64][2];
    int top = 0;
    auto push = [&](int x, int y) {
        if (x >= 0 && x < 64 && y >= 0 && y < 32 && !outside[y][x] && frame.pixels[y][x][3] <= 60) {
            outside[y][x] = true;
            stack[top][0] = x;
            stack[top][1] = y;
            top++;
        }
    };
    for (int x = 0; x < 64; x++) {
        push(x, 0);
        push(x, 31);
    }
    for (int y = 0; y < 32; y++) {
        push(0, y);
        push(63, y);
    }
    while (top > 0) {
        top--;
        const int x = stack[top][0];
        const int y = stack[top][1];
        push(x + 1, y);
        push(x - 1, y);
        push(x, y + 1);
        push(x, y - 1);
    }
    for (int y = 0; y < 32; y++) {
        for (int x = 0; x < 64; x++) {
            inside[y][x] = !outside[y][x];
        }
    }
    for (int pass = 0; pass < 4; pass++) {
        bool next[32][64];
        for (int y = 0; y < 32; y++) {
            for (int x = 0; x < 64; x++) {
                next[y][x] = inside[y][x] && x > 0 && x < 63 && y > 0 && y < 31 && inside[y][x - 1] &&
                             inside[y][x + 1] && inside[y - 1][x] && inside[y + 1][x];
            }
        }
        for (int y = 0; y < 32; y++) {
            for (int x = 0; x < 64; x++) {
                inside[y][x] = next[y][x];
            }
        }
    }
}

void build_sign(uint8_t* rdram, int sign, int variant) {
    const SignFrame& frame = g_frames[sign][variant];
    bool inside[32][64];
    find_inside(frame, inside);
    for (int y = 0; y < 32; y++) {
        for (int x = 0; x < 64; x++) {
            const uint8_t* src = inside[y][x] ? kLetters[sign][variant][y][x] : frame.pixels[y][x];
            const uint32_t tile = sign_tile(sign, variant, x / 32);
            const uint32_t offset = tile_offset(x % 32, y);
            for (int c = 0; c < 4; c++) {
                MEM_B(offset + c, kseg0(tile)) = static_cast<int8_t>(src[c]);
            }
        }
    }
    g_frames[sign][variant].ready = true;
}

void capture_tile(uint8_t* rdram, int sign, int index, uint32_t address) {
    const int variant = index / 2;
    const int half = index % 2;
    SignFrame& frame = g_frames[sign][variant];
    const gpr base = kseg0((address & 0x1FFFFFFF) | 0x80000000);
    for (int y = 0; y < 32; y++) {
        for (int x = 0; x < 32; x++) {
            const uint32_t offset = tile_offset(x, y);
            for (int c = 0; c < 4; c++) {
                frame.pixels[y][half * 32 + x][c] = MEM_BU(offset + c, base);
            }
        }
    }
    frame.captured |= 1u << half;
    if (frame.captured == 3) {
        build_sign(rdram, sign, variant);
    }
}

// func_1510D0EC(resource, out, 3, 0) as func_151ED430 calls it; returns the tile's address.
uint32_t look_up_tile(uint8_t* rdram, recomp_context* ctx, int resource) {
    recomp_context saved = *ctx;
    ctx->r4 = resource;
    ctx->r5 = ctx->r29 + 0x98;
    ctx->r6 = 3;
    ctx->r7 = 0;
    func_1510D0EC(rdram, ctx);
    const uint32_t address = static_cast<uint32_t>(ctx->r2);
    *ctx = saved;
    return address;
}

// --- drawing -------------------------------------------------------------------------------------

Clock::time_point g_saved_at;
bool g_saved = false;
Clock::time_point g_last_frame;

void write_string(uint8_t* rdram, uint32_t address, const char* text) {
    for (uint32_t i = 0;; i++) {
        MEM_B(i, kseg0(address)) = text[i];
        if (text[i] == '\0') {
            break;
        }
    }
}

// func_15042D94(x, y, 1, format, argument), as the menu draws its other labels.
void draw_label(uint8_t* rdram, recomp_context* ctx, int x, int y, uint32_t format, int argument) {
    recomp_context saved = *ctx;
    ctx->r4 = x;
    ctx->r5 = y;
    ctx->r6 = 1;
    ctx->r7 = kseg0(format);
    MEM_W(kOutgoingArg, ctx->r29) = argument;
    func_15042D94(rdram, ctx);
    *ctx = saved;
}

void draw_saved(uint8_t* rdram, recomp_context* ctx, int layout) {
    if (!g_saved) {
        return;
    }
    const double age = std::chrono::duration<double>(Clock::now() - g_saved_at).count();
    if (age >= kSavedHoldSeconds + kSavedFadeSeconds) {
        g_saved = false;
        return;
    }
    const double fade = age <= kSavedHoldSeconds ? 1.0 : 1.0 - (age - kSavedHoldSeconds) / kSavedFadeSeconds;
    write_string(rdram, kSavedText, "SAVED");
    const uint32_t color = static_cast<uint32_t>(MEM_W(0, kseg0(kTextColor)));
    const uint32_t alpha = static_cast<uint32_t>((color & 0xFF) * fade);
    MEM_W(0, kseg0(kTextColor)) = static_cast<int32_t>(0xFFFFFF00u | alpha);
    draw_label(rdram, ctx, kLeverX, layout + kSavedTextY, kSavedText, 0);
    MEM_W(0, kseg0(kTextColor)) = static_cast<int32_t>(color);
}

// --- the stick -----------------------------------------------------------------------------------

int axis_zone(int value) { return value > kStickEdge ? 1 : value < -kStickEdge ? -1 : 0; }

// Where a push in direction (dx, dy) leads from `from`; dy is up.
Option next_option(Option from, int dx, int dy) {
    if (dy < 0 && dx != 0) {
        return dx > 0 ? kSave : kLoad;  // the diagonals go straight to the lower signs
    }
    if (dy > 0 && dx != 0) {
        return dx > 0 ? kCont : kQuit;
    }
    const bool left = from == kQuit || from == kLoad;
    const bool lower = from == kSave || from == kLoad;
    if (dx != 0) {
        const bool to_left = dx < 0;
        return lower ? (to_left ? kLoad : kSave) : (to_left ? kQuit : kCont);
    }
    const bool to_lower = dy < 0;
    return left ? (to_lower ? kLoad : kQuit) : (to_lower ? kSave : kCont);
}

}  // namespace

extern "C" int conker_pause_save_skip_input = 0;

// At 0x151DE00C, after the layout (option count and offset) is chosen from D_800BE616.
extern "C" void conker_pause_save_layout(uint8_t* rdram, recomp_context* ctx) {
    if (!pause_save_enabled() || MEM_BU(0, kseg0(kChapterReplay)) != 0 || MEM_B(0, kseg0(kSaveSlot)) < 0 ||
        MEM_BU(kSpecialPauseSlot, ctx->r29) != 0) {
        return;
    }
    MEM_W(kOptionCountSlot, ctx->r29) = 3;
    // The Y/N page (a selection past the options) keeps the two-option layout, so QUIT sits
    // between PAUSED and the Y/N lever as it does without SAVE and LOAD.
    if (MEM_B(0, kseg0(kPauseSelection)) < 3) {
        MEM_W(kLayoutYSlot, ctx->r29) = MEM_W(kLayoutYSlot, ctx->r29) - kLift;
    }
    // A pause that starts afresh carries over neither "SAVED" nor LOAD.
    const Clock::time_point now = Clock::now();
    if (now - g_last_frame > std::chrono::milliseconds(250)) {
        g_saved = false;
        g_load_selected = false;
    }
    g_last_frame = now;
}

// At 0x151DE05C, the stick handling for the option page ($t0/$t1 stick X/Y). The game's
// three-option rules do not fit the new layout, so do it here and skip to 0x151DE16C, which
// stores the stick for next frame's edges.
extern "C" void conker_pause_save_input(uint8_t* rdram, recomp_context* ctx) {
    conker_pause_save_skip_input = 0;
    if (!signs_shown(rdram, ctx)) {
        return;
    }
    const int dx = axis_zone(static_cast<int8_t>(ctx->r8));
    const int dy = axis_zone(static_cast<int8_t>(ctx->r9));
    const int previous_dx = axis_zone(MEM_B(0, kseg0(kPreviousStickX)));
    const int previous_dy = axis_zone(MEM_B(0, kseg0(kPreviousStickY)));
    if ((dx != 0 || dy != 0) && (dx != previous_dx || dy != previous_dy)) {
        const Option from = current_option(rdram);
        const Option to = next_option(from, dx, dy);
        if (to != from) {
            select_option(rdram, to);
            MEM_W(kSoundSlot, ctx->r29) = kLeverSound;
        }
    }
    conker_pause_save_skip_input = 1;
}

// Before the `jal func_15042E3C` that draws RESTART ("#" with selector 0x21 or 0x22): draw LOAD
// and "SAVED", then put the RESTART sprite, which becomes the SAVE sign, below CONT....
extern "C" void conker_pause_save_sign(uint8_t* rdram, recomp_context* ctx) {
    if (!signs_shown(rdram, ctx)) {
        return;
    }
    const int layout = MEM_W(kLayoutYSlot, ctx->r29);
    write_string(rdram, kSpriteFormat, "#");
    draw_label(rdram, ctx, kLeftSignX, layout + kLowerSignY, kSpriteFormat,
               kLoadSelector + (current_option(rdram) == kLoad ? 1 : 0));
    draw_saved(rdram, ctx, layout);
    MEM_H(0, kseg0(kTextX)) = kRightSignX;
    MEM_H(0, kseg0(kTextY)) = static_cast<int16_t>(layout + kLowerSignY);
    g_signs_drawn = true;
}

// Before the `jal func_150432FC` that places QUIT below RESTART ($a1 = layout + 0x82): keep it
// where the two-option menu has it, level with CONT....
extern "C" void conker_pause_save_quit_position(uint8_t* rdram, recomp_context* ctx) {
    if (signs_shown(rdram, ctx)) {
        ctx->r5 = MEM_W(kLayoutYSlot, ctx->r29) + kSignY;
    }
}

// Before the `jal func_15042E3C` that draws QUIT ($a1 = 1 when selection 2, before adding 0xE):
// unlit while LOAD is the one selected.
extern "C" void conker_pause_save_quit_sign(uint8_t* rdram, recomp_context* ctx) {
    if (signs_shown(rdram, ctx) && current_option(rdram) == kLoad) {
        ctx->r5 = 0;
    }
}

// At 0x151DE3DC `beq $t9, $zero, 0x151DE698`, where A on RESTART or QUIT would open the Y/N page
// ($t9 = A pressed this frame). A on SAVE queues the save as func_15007718 does; A on LOAD starts
// the reload and closes the menu. Either way $t9 is cleared, so the branch skips to the end of
// the frame.
extern "C" void conker_pause_save_select(uint8_t* rdram, recomp_context* ctx) {
    if (!signs_shown(rdram, ctx) || ctx->r25 == 0) {
        return;
    }
    const Option option = current_option(rdram);
    const int8_t slot = MEM_B(0, kseg0(kSaveSlot));
    if (option == kSave) {
        ctx->r25 = 0;
        if (MEM_B(0, kseg0(kEepromAction)) == 0) {
            conker::resume::remember_position(rdram, slot);
            MEM_B(0, kseg0(kEepromAction)) = kEepromSave;
            MEM_B(0, kseg0(kEepromSlot)) = slot;
            MEM_W(kSoundSlot, ctx->r29) = kSelectSound;
            g_saved = true;
            g_saved_at = Clock::now();
        }
    } else if (option == kLoad) {
        ctx->r25 = 0;
        if (MEM_B(0, kseg0(kEepromAction)) == 0) {
            conker::resume::request_load(rdram, slot);
            MEM_W(kSoundSlot, ctx->r29) = kSelectSound;
            MEM_B(0, kseg0(kPauseClose)) = 5;
            MEM_B(0, kseg0(kPauseClosing)) = 10;
            g_load_selected = false;
        }
    }
}

// Before the `jal func_15007718` that saves when QUIT is confirmed ($a0 = slot): remember where
// Conker is, so quitting right after SAVE does not lose the spot.
extern "C" void conker_pause_save_quit_confirmed(uint8_t* rdram, recomp_context* ctx) {
    conker::resume::remember_position(rdram, static_cast<int8_t>(ctx->r4));
}

// In func_151ED430 right after `jal func_1510D0EC` ($s4 = resource, $v0 = its tile in RDRAM):
// keep copies of the CONT... and QUIT tiles, and give out the signs' tiles in place of the
// RESTART and PLAY ones.
extern "C" void conker_pause_save_tile(uint8_t* rdram, recomp_context* ctx) {
    if (!g_signs_drawn) {
        return;
    }
    const int resource = static_cast<int32_t>(ctx->r20);
    const uint32_t address = static_cast<uint32_t>(ctx->r2);
    for (int sign = 0; sign < 2; sign++) {
        const int source = resource - kSigns[sign].source_tiles;
        if (source >= 0 && source < 4) {
            if (address != kNotLoaded) {
                capture_tile(rdram, sign, source, address);
            }
            // A sign is lit only once its option is chosen; fetch the lit tile with the unlit one
            // so the lit sign is ready by then.
            if (source < 2 && !g_frames[sign][1].ready) {
                const uint32_t lit = look_up_tile(rdram, ctx, resource + 2);
                if (lit != kNotLoaded) {
                    capture_tile(rdram, sign, source + 2, lit);
                }
            }
            return;
        }
        const int target = resource - kSigns[sign].target_tiles;
        if (target >= 0 && target < 4) {
            const int variant = target / 2;
            ctx->r2 = g_frames[sign][variant].ready ? kseg0(sign_tile(sign, variant, target % 2)) : kseg0(kNotLoaded);
            return;
        }
    }
}

// At the end of func_15043384, which draws the frame's sprite list.
extern "C" void conker_pause_save_sprites_done(uint8_t* /*rdram*/, recomp_context* /*ctx*/) {
    g_signs_drawn = false;
}

nlohmann::json conker::status::pause_save() {
    return enhancement("pause_save", "SAVE and LOAD in the pause menu", pause_save_enabled(), "CONKER_PAUSE_SAVE");
}
