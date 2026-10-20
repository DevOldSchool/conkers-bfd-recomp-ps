// The debug menu: the pad's Back button (Create/Share on a PlayStation pad, View on an Xbox pad)
// or the ` key opens it during play. Native helpers called from the [[patches.hook]] snippets in
// config/us.toml (func_151E51EC, func_151DDC20, func_151EB06C).
//
// It is the game's own pause with its page replaced, so the game stops just as it does for the
// pause menu and the menu draws with the game's font. A press asks for the pause: at the start of
// func_151DDC20, the in-play frame handler, Start is added to controller 1's buttons pressed this
// frame (D_800BE930), so the pause opens only where Start would open it, and taken out again once
// the handler has looked for it. When that pause opened (D_800BEAC1), the pause page
// (0x151DE00C on) draws this menu and skips to the end of the frame, 0x151DE698, which plays the
// sound left in the frame's sound slot. func_151EB06C, which draws PAUSED and the HUD while
// paused, is kept from drawing them. Closing it is CONT...'s: the close countdown in D_8008FEEC/F0.
//
//     WIREFRAME        the game's 3D triangles drawn as their edges (src/platform/render_debug.hpp)
//     SHOW FPS         the frames the game draws per second, in the corner, on every screen
//     SHOW POSITION    Conker's scene, position and facing, and the camera's yaw and pitch, under
//                      it, during play
//     TELEPORT         X, Y and Z to move him to, and spots kept in conker-recomp-data/debug_spots.txt
//     LOAD SNAPSHOT    a list of saves/snapshots/; A starts one (resume_point.cpp)
//
// The first page also shows where Conker is and where the camera looks, and opening the menu prints
// them (with the camera's position, look-at point and forward vector). Up and down choose,
// A or left and right change (held, a number keeps changing), B goes back, and Back, Start or B on
// the first page close it. The pad is read directly (conker::input::read), not through the game.
// CONKER_DEBUG_MENU=0 turns it off.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "recomp.h"
#include "enhancements/pause_save/resume_point.hpp"
#include "platform/input.hpp"
#include "platform/render_debug.hpp"
#include "ultramodern/ultramodern.hpp"

extern "C" void func_15042D94(uint8_t* rdram, recomp_context* ctx);

namespace {

using conker::resume::Position;

inline gpr kseg0(uint32_t address) { return static_cast<gpr>(static_cast<int32_t>(address)); }

constexpr uint32_t kMenuHandler = 0x800E0B94;   // D_800E0B94: func_151E51EC's handler, 0 in play
constexpr uint32_t kPadPressed = 0x800BE930;    // D_800BE930: buttons pressed this frame, per pad
constexpr uint32_t kPauseOpen = 0x800BEAC1;     // D_800BEAC1: set when Start opens the pause
constexpr uint32_t kPauseClosing = 0x8008FEEC;  // D_8008FEEC/F0: set to 10 and 5 to close it,
constexpr uint32_t kPauseClose = 0x8008FEF0;    // as A or B on CONT... does
constexpr uint32_t kTextColor = 0x800CBD60;     // D_800CBD60: RGBA given to the next text record
constexpr uint32_t kText = 0x807FB000;          // free RDRAM (see pause_save.cpp): the format string
constexpr int kTextSize = 0x40;                 // a text record holds 63 characters
constexpr int kOutgoingArg = 0x10;              // the caller's frame: first stack argument
constexpr int kSoundSlot = 0x84;                // func_151DDC20's frame: the sound to play

constexpr uint16_t kStartButton = 0x1000;
constexpr uint8_t kLeft = 0x80;                 // text flags: left-aligned at x,
constexpr uint8_t kCentered = 0x81;             // or centered on it
constexpr int kCenterX = 0x92;                  // the middle of the 292-pixel picture
constexpr int32_t kMoveSound = 0x62D;           // the pause lever's
constexpr int32_t kSelectSound = 0x500;
constexpr uint32_t kTitleColor = 0xCEC461FF;    // the game's gold
constexpr uint32_t kChosenColor = 0xFFFFFFFF;
constexpr uint32_t kOtherColor = 0x9A9A9AFF;
constexpr int kTitleY = 46;
constexpr int kFirstRowY = 64;
constexpr int kRowHeight = 13;
constexpr int kListRows = 9;
constexpr int kInfoY = 178;                     // the position and camera lines under the rows
constexpr int kOverlayX = 16;                   // FPS and position in play, top left
constexpr int kOverlayY = 12;
constexpr int kSteps[] = {1, 10, 100, 1000};

using Clock = std::chrono::steady_clock;

enum class Page { Main, Teleport, Spots, Snapshots };
enum MainRow { kWireframe, kShowFps, kShowPosition, kTeleport, kLoadSnapshot, kMainRows };
enum TeleportRow { kX, kY, kZ, kStep, kGo, kHere, kRemember, kSpotList, kTeleportRows };

bool g_open_requested = false;
bool g_injected = false;
uint16_t g_pad_before = 0;
bool g_active = false;
bool g_close_requested = false;
Page g_page = Page::Main;
int g_row = 0;           // on the first page
int g_teleport_row = 0;
int g_entry = 0;         // in a list
int g_top = 0;
std::vector<conker::resume::Snapshot> g_snapshots;
std::vector<Position> g_spots;  // this scene's
std::string g_message;
Clock::time_point g_message_until;

bool g_wireframe = false;
bool g_show_fps = false;
bool g_show_position = false;
int g_frames = 0;
Clock::time_point g_fps_since = Clock::now();
int g_fps = 0;

Position g_target;       // the teleport page's X, Y and Z
int g_step = 2;          // index in kSteps

uint16_t g_previous_buttons = 0;

bool debug_menu_enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("CONKER_DEBUG_MENU");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}

void show_message(const std::string& message) {
    g_message = message;
    g_message_until = Clock::now() + std::chrono::seconds(2);
}

// --- spots ---------------------------------------------------------------------------------------

// conker-recomp-data/debug_spots.txt: one spot a line, "<scene> <x> <y> <z> <facing>".
std::filesystem::path spots_path() {
    return ultramodern::get_save_file_path().parent_path().parent_path() / "debug_spots.txt";
}

std::vector<Position> read_spots() {
    std::vector<Position> spots;
    std::ifstream file(spots_path());
    for (std::string line; std::getline(file, line);) {
        Position spot;
        if (std::sscanf(line.c_str(), "%d %f %f %f %f", &spot.scene, &spot.x, &spot.y, &spot.z, &spot.yaw) == 5) {
            spots.push_back(spot);
        }
    }
    return spots;
}

std::vector<Position> scene_spots(int scene) {
    std::vector<Position> spots;
    for (const Position& spot : read_spots()) {
        if (spot.scene == scene) {
            spots.push_back(spot);
        }
    }
    return spots;
}

void remember_spot(const Position& spot) {
    std::ofstream file(spots_path(), std::ios::app);
    char line[96];
    std::snprintf(line, sizeof(line), "%d %.2f %.2f %.2f %.2f\n", spot.scene, spot.x, spot.y, spot.z, spot.yaw);
    file << line;
    std::printf("[conker] debug menu: spot kept, scene %d (%.0f, %.0f, %.0f)\n", spot.scene, spot.x, spot.y, spot.z);
}

std::string describe(const Position& p) {
    char text[kTextSize];
    std::snprintf(text, sizeof(text), "%.0f  %.0f  %.0f", p.x, p.y, p.z);
    return text;
}

// --- drawing -------------------------------------------------------------------------------------

// func_15042D94(x, y, flags, format), as the game queues its HUD text.
void draw_text(uint8_t* rdram, recomp_context* ctx, int x, int y, uint8_t flags, uint32_t color, const std::string& text) {
    const size_t length = std::min<size_t>(text.size(), kTextSize - 1);
    for (size_t i = 0; i < length; i++) {
        const char c = text[i] == '%' || text[i] == '#' ? ' ' : text[i];  // format characters
        MEM_B(static_cast<int>(i), kseg0(kText)) = c;
    }
    MEM_B(static_cast<int>(length), kseg0(kText)) = 0;
    const int32_t previous_color = MEM_W(0, kseg0(kTextColor));
    MEM_W(0, kseg0(kTextColor)) = static_cast<int32_t>(color);
    recomp_context saved = *ctx;
    ctx->r4 = x;
    ctx->r5 = y;
    ctx->r6 = flags;
    ctx->r7 = kseg0(kText);
    MEM_W(kOutgoingArg, ctx->r29) = 0;
    func_15042D94(rdram, ctx);
    *ctx = saved;
    MEM_W(0, kseg0(kTextColor)) = previous_color;
}

const char* on_off(bool on) { return on ? "ON" : "OFF"; }

std::string position_line(uint8_t* rdram) {
    Position p;
    if (!conker::resume::current_position(rdram, &p)) {
        return "";
    }
    char text[kTextSize];
    std::snprintf(text, sizeof(text), "SCENE %d  %.0f %.0f %.0f  FACING %.0f", p.scene, p.x, p.y, p.z, p.facing);
    return text;
}

std::string camera_line(uint8_t* rdram) {
    conker::resume::Camera camera;
    if (!conker::resume::current_camera(rdram, &camera)) {
        return "";
    }
    char text[kTextSize];
    std::snprintf(text, sizeof(text), "CAMERA YAW %.0f  PITCH %.0f", camera.yaw(), camera.pitch());
    return text;
}

// The title, or for two seconds the last message in its place.
void draw_title(uint8_t* rdram, recomp_context* ctx, const std::string& title) {
    const bool message = !g_message.empty() && Clock::now() < g_message_until;
    draw_text(rdram, ctx, kCenterX, kTitleY, kCentered, kTitleColor, message ? g_message : title);
}

void draw_rows(uint8_t* rdram, recomp_context* ctx, const std::vector<std::string>& rows, int chosen, int first = 0) {
    const int count = static_cast<int>(rows.size());
    for (int i = first; i < std::min(count, first + kListRows); i++) {
        draw_text(rdram, ctx, kCenterX, kFirstRowY + (i - first) * kRowHeight, kCentered,
                  i == chosen ? kChosenColor : kOtherColor, rows[i]);
    }
}

void draw_page(uint8_t* rdram, recomp_context* ctx) {
    char text[kTextSize];
    switch (g_page) {
        case Page::Main:
            draw_title(rdram, ctx, "DEBUG MENU");
            draw_rows(rdram, ctx,
                      {std::string("WIREFRAME  ") + on_off(g_wireframe), std::string("SHOW FPS  ") + on_off(g_show_fps),
                       std::string("SHOW POSITION  ") + on_off(g_show_position), "TELEPORT", "LOAD SNAPSHOT"},
                      g_row);
            break;
        case Page::Teleport:
            draw_title(rdram, ctx, "TELEPORT");
            std::snprintf(text, sizeof(text), "%d", kSteps[g_step]);
            draw_rows(rdram, ctx,
                      {"X  " + std::to_string(static_cast<int>(std::lround(g_target.x))),
                       "Y  " + std::to_string(static_cast<int>(std::lround(g_target.y))),
                       "Z  " + std::to_string(static_cast<int>(std::lround(g_target.z))), std::string("STEP  ") + text, "GO",
                       "HERE", "REMEMBER", "SPOTS  " + std::to_string(g_spots.size())},
                      g_teleport_row);
            break;
        case Page::Spots: {
            const int count = static_cast<int>(g_spots.size());
            std::snprintf(text, sizeof(text), count > 0 ? "SPOTS  %d/%d" : "SPOTS", g_entry + 1, count);
            draw_title(rdram, ctx, text);
            if (count == 0) {
                draw_rows(rdram, ctx, {"NONE IN THIS SCENE", "REMEMBER ONE ON TELEPORT"}, -1);
            }
            std::vector<std::string> rows;
            for (const Position& spot : g_spots) {
                rows.push_back(describe(spot));
            }
            draw_rows(rdram, ctx, rows, g_entry, g_top);
            break;
        }
        case Page::Snapshots: {
            const int count = static_cast<int>(g_snapshots.size());
            std::snprintf(text, sizeof(text), count > 0 ? "LOAD SNAPSHOT  %d/%d" : "LOAD SNAPSHOT", g_entry + 1, count);
            draw_title(rdram, ctx, text);
            if (count == 0) {
                draw_rows(rdram, ctx, {"NO SNAPSHOTS YET", "SAVE FROM THE PAUSE MENU"}, -1);
            }
            std::vector<std::string> rows;
            for (const conker::resume::Snapshot& snapshot : g_snapshots) {
                std::snprintf(text, sizeof(text), "SCENE %-3d %s", snapshot.scene, snapshot.when.c_str());
                rows.push_back(text);
            }
            draw_rows(rdram, ctx, rows, g_entry, g_top);
            break;
        }
    }
    if (g_page == Page::Main || g_page == Page::Teleport) {
        draw_text(rdram, ctx, kCenterX, kInfoY, kCentered, kOtherColor, position_line(rdram));
        draw_text(rdram, ctx, kCenterX, kInfoY + 11, kCentered, kOtherColor, camera_line(rdram));
    }
}

// --- input ---------------------------------------------------------------------------------------

// A direction on one axis: a fresh push, then repeats while held.
struct Repeat {
    int previous = 0;
    Clock::time_point next;

    // -1, 1 or 0; `fresh` is set only for the push itself.
    int step(int direction, bool* fresh) {
        const Clock::time_point now = Clock::now();
        int step = 0;
        *fresh = false;
        if (direction != 0 && direction != previous) {
            step = direction;
            *fresh = true;
            next = now + std::chrono::milliseconds(400);
        } else if (direction != 0 && now >= next) {
            step = direction;
            next = now + std::chrono::milliseconds(80);
        }
        previous = direction;
        return step;
    }
};

Repeat g_vertical;
Repeat g_horizontal;

void close_menu(uint8_t* rdram) {
    g_active = false;
    g_close_requested = false;
    MEM_B(0, kseg0(kPauseClose)) = 5;
    MEM_B(0, kseg0(kPauseClosing)) = 10;
}

void set_wireframe(bool on) {
    g_wireframe = on;
    conker::renderer::set_wireframe(on);
    std::printf("[conker] debug menu: wireframe %s\n", on ? "on" : "off");
}

// Moves the list selection by `step` and keeps it in view.
void move_in_list(int step, int count) {
    g_entry = std::clamp(g_entry + step, 0, std::max(count - 1, 0));
    g_top = std::clamp(g_top, g_entry - kListRows + 1, g_entry);
}

void open_list(Page page) {
    g_page = page;
    g_entry = 0;
    g_top = 0;
}

// The first page. Returns the sound to play.
int32_t main_page(uint8_t* rdram, int vertical, int horizontal, bool fresh_horizontal, uint16_t pressed) {
    if (vertical != 0) {
        g_row = (g_row + vertical + kMainRows) % kMainRows;
        return kMoveSound;
    }
    const bool change = (pressed & conker::input::kA) || (horizontal != 0 && fresh_horizontal);
    if (!change) {
        return 0;
    }
    switch (g_row) {
        case kWireframe:
            set_wireframe(!g_wireframe);
            break;
        case kShowFps:
            g_show_fps = !g_show_fps;
            break;
        case kShowPosition:
            g_show_position = !g_show_position;
            break;
        case kTeleport:
            if (!(pressed & conker::input::kA)) {
                return 0;
            }
            if (!conker::resume::current_position(rdram, &g_target)) {
                show_message("NO CONKER HERE");
                return kSelectSound;
            }
            g_spots = scene_spots(g_target.scene);
            g_page = Page::Teleport;
            break;
        case kLoadSnapshot:
            if (!(pressed & conker::input::kA)) {
                return 0;
            }
            g_snapshots = conker::resume::list_snapshots();
            open_list(Page::Snapshots);
            break;
    }
    return kSelectSound;
}

int32_t teleport_page(uint8_t* rdram, int vertical, int horizontal, bool fresh_horizontal, uint16_t pressed) {
    if (pressed & conker::input::kB) {
        g_page = Page::Main;
        return kSelectSound;
    }
    if (vertical != 0) {
        g_teleport_row = (g_teleport_row + vertical + kTeleportRows) % kTeleportRows;
        return kMoveSound;
    }
    float* axis = g_teleport_row == kX ? &g_target.x : g_teleport_row == kY ? &g_target.y : g_teleport_row == kZ ? &g_target.z : nullptr;
    if (axis != nullptr && horizontal != 0) {
        *axis = static_cast<float>(std::lround(*axis)) + horizontal * kSteps[g_step];
        return fresh_horizontal ? kMoveSound : 0;
    }
    if (g_teleport_row == kStep && horizontal != 0 && fresh_horizontal) {
        g_step = std::clamp(g_step + horizontal, 0, static_cast<int>(std::size(kSteps)) - 1);
        return kMoveSound;
    }
    if (!(pressed & conker::input::kA)) {
        return 0;
    }
    Position here;
    switch (g_teleport_row) {
        case kStep:
            g_step = (g_step + 1) % static_cast<int>(std::size(kSteps));
            break;
        case kGo:
            if (conker::resume::teleport(rdram, g_target)) {
                close_menu(rdram);
            } else {
                show_message("CANNOT TELEPORT HERE");
            }
            break;
        case kHere:
            if (conker::resume::current_position(rdram, &here)) {
                g_target = here;
            }
            break;
        case kRemember:
            remember_spot(g_target);
            g_spots = scene_spots(g_target.scene);
            show_message("SPOT KEPT");
            break;
        case kSpotList:
            open_list(Page::Spots);
            break;
        default:
            return 0;
    }
    return kSelectSound;
}

int32_t list_page(uint8_t* rdram, int vertical, uint16_t pressed) {
    const int count = static_cast<int>(g_page == Page::Spots ? g_spots.size() : g_snapshots.size());
    if (pressed & conker::input::kB) {
        g_page = g_page == Page::Spots ? Page::Teleport : Page::Main;
        return kSelectSound;
    }
    if (vertical != 0 && count > 0) {
        move_in_list(vertical, count);
        return kMoveSound;
    }
    if (!(pressed & conker::input::kA) || count == 0) {
        return 0;
    }
    if (g_page == Page::Spots) {
        g_target = g_spots[g_entry];
        if (conker::resume::teleport(rdram, g_target)) {
            close_menu(rdram);
        } else {
            show_message("CANNOT TELEPORT HERE");
        }
    } else if (conker::resume::load_snapshot(rdram, g_snapshots[g_entry].path)) {
        close_menu(rdram);
    } else {
        show_message("SAVING, TRY AGAIN");
    }
    return kSelectSound;
}

}  // namespace

// In func_151E51EC, the menu handler, once a frame on every screen (after func_15042D78): counts
// frames, draws the frame rate and position, and takes a press of the debug menu button.
extern "C" void conker_debug_menu_tick(uint8_t* rdram, recomp_context* ctx) {
    g_frames++;
    const Clock::time_point now = Clock::now();
    const double elapsed = std::chrono::duration<double>(now - g_fps_since).count();
    if (elapsed >= 0.5) {
        g_fps = static_cast<int>(g_frames / elapsed + 0.5);
        g_frames = 0;
        g_fps_since = now;
    }
    int y = kOverlayY;
    if (g_show_fps) {
        draw_text(rdram, ctx, kOverlayX, y, kLeft, 0xFFFFFFFF, std::to_string(g_fps) + " FPS");
        y += 11;
    }
    if (g_show_position && !g_active) {
        draw_text(rdram, ctx, kOverlayX, y, kLeft, 0xFFFFFFFF, position_line(rdram));
        draw_text(rdram, ctx, kOverlayX, y + 11, kLeft, 0xFFFFFFFF, camera_line(rdram));
    }

    if (g_active && MEM_BU(0, kseg0(kPauseOpen)) == 0) {
        g_active = false;  // the pause went away by itself
    }
    if (conker::input::take_debug_menu_press() && debug_menu_enabled()) {
        if (g_active) {
            g_close_requested = true;
        } else if (MEM_BU(0, kseg0(kMenuHandler)) == 0) {
            g_open_requested = true;
        }
    }
}

// At the start of func_151DDC20: press Start for the pause the menu lives in.
extern "C" void conker_debug_menu_request(uint8_t* rdram, recomp_context* /*ctx*/) {
    if (!g_open_requested || g_active || MEM_BU(0, kseg0(kPauseOpen)) != 0) {
        g_open_requested = false;
        return;
    }
    g_pad_before = MEM_HU(0, kseg0(kPadPressed));
    MEM_H(0, kseg0(kPadPressed)) = static_cast<int16_t>(g_pad_before | kStartButton);
    g_injected = true;
}

// At 0x151DDDE0, once func_151DDC20 has looked for Start: take it out again, and if the pause
// opened, it is the debug menu's.
extern "C" void conker_debug_menu_requested(uint8_t* rdram, recomp_context* /*ctx*/) {
    if (!g_injected) {
        return;
    }
    g_injected = false;
    g_open_requested = false;
    MEM_H(0, kseg0(kPadPressed)) = static_cast<int16_t>(g_pad_before);
    if (MEM_BU(0, kseg0(kPauseOpen)) != 0) {
        g_active = true;
        g_close_requested = false;
        g_page = Page::Main;
        g_message.clear();
        uint16_t buttons;
        float x, y;
        conker::input::read(&buttons, &x, &y);
        g_previous_buttons = buttons;  // a button held while opening does not count as a press
        Position p;
        if (conker::resume::current_position(rdram, &p)) {
            std::printf("[conker] debug menu: scene %d at (%.2f, %.2f, %.2f) facing %.2f\n", p.scene, p.x, p.y, p.z, p.facing);
        }
        conker::resume::Camera c;
        if (conker::resume::current_camera(rdram, &c)) {
            std::printf("[conker] debug menu: camera at (%.2f, %.2f, %.2f) looking at (%.2f, %.2f, %.2f), forward (%.3f, %.3f, "
                        "%.3f), yaw %.2f pitch %.2f\n",
                        c.eye[0], c.eye[1], c.eye[2], c.at[0], c.at[1], c.at[2], c.forward[0], c.forward[1], c.forward[2],
                        c.yaw(), c.pitch());
        }
    }
}

// In func_151EB06C, which draws PAUSED and the HUD (chocolate, cash) while the game is paused, at
// 0x151EB128 ($v0 = D_800BEAC0, the pause running): the debug menu's pause shows neither, so the
// game is not paused as far as it is concerned.
extern "C" void conker_debug_menu_hud(uint8_t* /*rdram*/, recomp_context* ctx) {
    if (g_active) {
        ctx->r2 = 0;
    }
}

extern "C" int conker_debug_menu_skip = 0;

// At 0x151DE00C, where the pause page starts: while it is the debug menu's, run and draw the menu
// instead and set conker_debug_menu_skip, to skip to the end of the frame.
extern "C" void conker_debug_menu_frame(uint8_t* rdram, recomp_context* ctx) {
    conker_debug_menu_skip = g_active ? 1 : 0;
    if (!g_active) {
        return;
    }
    uint16_t buttons;
    float x, y;
    conker::input::read(&buttons, &x, &y);
    const uint16_t pressed = buttons & ~g_previous_buttons;
    g_previous_buttons = buttons;
    bool fresh_vertical, fresh_horizontal;
    const int up_down = ((buttons & conker::input::kDUp) || y > 0.5f) ? -1 : ((buttons & conker::input::kDDown) || y < -0.5f) ? 1 : 0;
    const int left_right = ((buttons & conker::input::kDLeft) || x < -0.5f) ? -1 : ((buttons & conker::input::kDRight) || x > 0.5f) ? 1 : 0;
    const int vertical = g_vertical.step(up_down, &fresh_vertical);
    const int horizontal = g_horizontal.step(left_right, &fresh_horizontal);

    if (g_close_requested || (pressed & conker::input::kStart) ||
        (g_page == Page::Main && (pressed & conker::input::kB))) {
        close_menu(rdram);
        MEM_W(kSoundSlot, ctx->r29) = kSelectSound;
        return;
    }

    int32_t sound = 0;
    switch (g_page) {
        case Page::Main:
            sound = main_page(rdram, vertical, horizontal, fresh_horizontal, pressed);
            break;
        case Page::Teleport:
            sound = teleport_page(rdram, vertical, horizontal, fresh_horizontal, pressed);
            break;
        case Page::Spots:
        case Page::Snapshots:
            sound = list_page(rdram, vertical, pressed);
            break;
    }
    if (!g_active) {  // a teleport or a load closed it
        MEM_W(kSoundSlot, ctx->r29) = sound;
        return;
    }
    draw_page(rdram, ctx);
    if (sound != 0) {
        MEM_W(kSoundSlot, ctx->r29) = sound;
    }
}
