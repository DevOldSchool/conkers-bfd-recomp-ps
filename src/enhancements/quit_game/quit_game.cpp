// EXIT GAME on the title menu (see quit_game.hpp).
//
// The title is screen 3 (D_800E0B94, func_151DE8F0): one room the camera turns around, GAME 1, MULTI,
// OPTIONS and CHAPTERS, and A zooms into the corner's page. Its state was found by dumping RDRAM
// during scripted presses and comparing the dumps taken while choosing a corner with those taken
// with a page open and on the PRESS START title before it:
//   s16 D_8008FEFC  above 0 while the camera sweeps into the room (func_151DE8F0 draws its
//       PRESS START text by it), 0 once the corners can be chosen
//   s8  D_800E0A94  -1 while a page is open (or opening), else 0
//   f32 D_800E0AF8  0 at the corners, 1 once zoomed into a page (also the MULTI barrels' turn)
//   u8  D_800E0B2E  1 in MULTI's barrel room, u8 D_800E0B1A nonzero while the camera moves
//   u8  D_800E0B1B/1C  the corner shown and the one being turned to (5 GAME 1, 0 MULTI, 1 OPTIONS,
//       2 CHAPTERS)
// Start opens the menu only when all of them say a corner is being chosen.
#include "enhancements/quit_game/quit_game.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "enhancements/debug_menu/debug_menu.hpp"
#include "enhancements/status.hpp"
#include "enhancements/switches.hpp"
#include "net/net_play.hpp"
#include "platform/input.hpp"
#include "platform/window.hpp"

namespace {

inline gpr kseg0(uint32_t address) { return static_cast<gpr>(static_cast<int32_t>(address)); }

constexpr uint32_t kScreen = 0x800E0B94;        // D_800E0B94: the screen's handler, 3 the title
constexpr int kTitleScreen = 3;
constexpr uint32_t kPressStart = 0x8008FEFC;     // above 0 while the camera sweeps in
constexpr uint32_t kPageOpen = 0x800E0A94;      // -1 while a page is open
constexpr uint32_t kZoom = 0x800E0AF8;          // f32: 0 at the corners
constexpr uint32_t kBarrelRoom = 0x800E0B2E;
constexpr uint32_t kCameraMoving = 0x800E0B1A;
constexpr uint32_t kCornerShown = 0x800E0B1B;
constexpr uint32_t kCornerTarget = 0x800E0B1C;

constexpr uint32_t kPadPressed = 0x800BE930;    // D_800BE930: controller 1's presses this frame
constexpr uint16_t kStartButton = 0x1000;

// The menu, in the 292x216 picture, in the middle under the corner's sign.
constexpr int kFirstRowY = 92;
constexpr int kRowHeight = 16;
constexpr int kAnswerSpread = 18;
constexpr uint32_t kLitColor = 0xFFFFFFFF;
constexpr uint32_t kUnlitColor = 0x9A9A9AFF;
constexpr float kStickEdge = 0.5f;

std::atomic<bool> g_enabled{[] {
    const char* value = std::getenv("CONKER_QUIT_GAME");
    return value == nullptr || value[0] != '0';
}()};  // the debug menu switches it

std::atomic<bool> g_quitting{false};

enum Row { kResume, kExit, kRows };

bool g_open = false;        // the menu is up
int g_row = kResume;
bool g_asking = false;      // EXIT GAME? is up
bool g_yes = false;         // and YES is the answer chosen
bool g_pads_held = false;   // the game reads no pad until controller 1's buttons are let go
uint16_t g_previous_buttons = 0;
int g_previous_x = 0;
int g_previous_y = 0;

bool quit_game_enabled() { return g_enabled.load(std::memory_order_relaxed); }

bool choosing_corner(uint8_t* rdram) {
    const uint32_t zoom = static_cast<uint32_t>(MEM_W(0, kseg0(kZoom)));
    return MEM_BU(0, kseg0(kScreen)) == kTitleScreen && MEM_H(0, kseg0(kPressStart)) <= 0 &&
           MEM_B(0, kseg0(kPageOpen)) == 0 && zoom == 0 && MEM_BU(0, kseg0(kBarrelRoom)) == 0 &&
           MEM_BU(0, kseg0(kCameraMoving)) == 0 &&
           MEM_BU(0, kseg0(kCornerShown)) == MEM_BU(0, kseg0(kCornerTarget));
}

int zone(float value) { return value > kStickEdge ? 1 : value < -kStickEdge ? -1 : 0; }

void close_menu() {
    g_open = false;
    g_asking = false;
}

}  // namespace

bool conker::quit_game::offered() {
    return quit_game_enabled() && !conker::net::active() && !conker::net::in_place();
}

void conker::quit_game::frame(uint8_t* rdram, recomp_context* ctx) {
    uint16_t buttons;
    float x, y;
    conker::input::read_menu(&buttons, &x, &y);
    const uint16_t pressed = buttons & ~g_previous_buttons;
    g_previous_buttons = buttons;
    // Up and down (+y is up, as the game reads it) and left and right, from the stick or the d-pad.
    const int dx = (buttons & conker::input::kDLeft) ? -1 : (buttons & conker::input::kDRight) ? 1 : zone(x);
    const int dy = (buttons & conker::input::kDUp) ? 1 : (buttons & conker::input::kDDown) ? -1 : zone(y);
    const bool fresh_x = dx != 0 && dx != g_previous_x;
    const bool fresh_y = dy != 0 && dy != g_previous_y;
    g_previous_x = dx;
    g_previous_y = dy;
    if (g_pads_held && !g_open && buttons == 0 && dx == 0 && dy == 0) {
        g_pads_held = false;
    }

    if (!offered() || !choosing_corner(rdram)) {
        close_menu();
        return;
    }

    bool select_sound = false;
    bool move_sound = false;
    if (!g_open) {
        // Start on controller 1 (pad or keyboard) opens the menu. It is read here, not from the
        // game's presses: on GAME 1 before the room has turned, D_800BE930 does not show it in the
        // frame this sees it. It is taken out of this frame's presses before the title's handler
        // reads them (func_151E51EC copies D_800BE930 after the tick), and from then on the game
        // gets empty pads, so it does not open the corner.
        if ((pressed & conker::input::kStart) == 0) {
            return;
        }
        MEM_H(0, kseg0(kPadPressed)) = static_cast<int16_t>(MEM_HU(0, kseg0(kPadPressed)) & ~kStartButton);
        g_open = true;
        g_row = kResume;
        g_asking = false;
        g_pads_held = true;
        select_sound = true;
    } else if (!g_asking) {
        if ((pressed & (conker::input::kB | conker::input::kStart)) ||
            ((pressed & conker::input::kA) && g_row == kResume)) {
            close_menu();  // the game's pads stay empty until the buttons are let go
            select_sound = true;
        } else if (pressed & conker::input::kA) {
            g_asking = true;
            g_yes = false;
            select_sound = true;
        } else if (fresh_y) {
            g_row = (g_row + (dy < 0 ? 1 : kRows - 1)) % kRows;
            move_sound = true;
        }
    } else {
        if (pressed & (conker::input::kB | conker::input::kStart)) {
            g_asking = false;
            move_sound = true;
        } else if (fresh_x && g_yes != (dx > 0)) {
            g_yes = dx > 0;
            move_sound = true;
        } else if (pressed & conker::input::kA) {
            select_sound = true;
            if (g_yes) {
                quit();
            } else {
                g_asking = false;
            }
        }
    }

    using conker::debug_menu::draw_text;
    using conker::debug_menu::kPictureCenterX;
    if (g_open && !g_asking) {
        draw_text(rdram, ctx, kPictureCenterX, kFirstRowY, true, g_row == kResume ? kLitColor : kUnlitColor, "RESUME", 1.0f);
        draw_text(rdram, ctx, kPictureCenterX, kFirstRowY + kRowHeight, true, g_row == kExit ? kLitColor : kUnlitColor,
                  "EXIT GAME", 1.0f);
    } else if (g_open) {
        draw_text(rdram, ctx, kPictureCenterX, kFirstRowY, true, kLitColor, "EXIT GAME?", 1.0f);
        draw_text(rdram, ctx, kPictureCenterX - kAnswerSpread, kFirstRowY + kRowHeight, true,
                  g_yes ? kUnlitColor : kLitColor, "NO", 1.0f);
        draw_text(rdram, ctx, kPictureCenterX + kAnswerSpread, kFirstRowY + kRowHeight, true,
                  g_yes ? kLitColor : kUnlitColor, "YES", 1.0f);
    }
    if (select_sound || move_sound) {
        conker::debug_menu::play_menu_sound(rdram, ctx, select_sound);
    }
}

bool conker::quit_game::holds_pads() { return g_open || g_pads_held; }

void conker::quit_game::quit() {
    if (g_quitting.exchange(true)) {
        return;
    }
    std::printf("[conker] EXIT GAME: closing the game\n");
    conker::window::request_quit();
}

nlohmann::json conker::status::quit_game() {
    return enhancement("quit_game", "EXIT GAME on the title menu", quit_game_enabled(), "CONKER_QUIT_GAME");
}

conker::switches::Switch conker::switches::quit_game() {
    return {"EXIT GAME", quit_game_enabled, [](bool on) { g_enabled.store(on); }};
}

