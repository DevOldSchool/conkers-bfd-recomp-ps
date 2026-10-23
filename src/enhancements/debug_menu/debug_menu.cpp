// The debug menu: the pad's Back button (Create/Share on a PlayStation pad, View on an Xbox pad)
// or the ` key opens it anywhere: in play, on the title and the game's menus, in a cutscene. Native
// helpers called from the [[patches.hook]] snippets in config/us.toml (func_151E51EC, func_151DDC20,
// func_151EB06C, func_1501BBB8).
//
// In play it is the game's own pause with its page replaced, so the game stops just as it does for
// the pause menu and the menu draws with the game's font. A press asks for the pause: at the start
// of func_151DDC20, the in-play frame handler, Start is added to controller 1's buttons pressed
// this frame (D_800BE930), and taken out again once the handler has looked for it. When that pause
// opened (D_800BEAC1), the pause page (0x151DE00C on) draws this menu and skips to the end of the
// frame, 0x151DE698, which plays the sound left in the frame's sound slot. func_151EB06C, which
// draws PAUSED and the HUD while paused, is kept from drawing them. Closing it is CONT...'s: the
// close countdown in D_8008FEEC/F0. A press while the pause menu is up turns it into this menu.
//
// Where Start does not open the pause (the title and the game's menus, a cutscene), the menu holds
// the game itself: it sets D_800BEAC1, which stops the world as the pause does (func_1501878C skips
// the world's frame and pauses the sound while D_800BEAC0, its copy, is set), keeps the screen's
// own handler from running (func_151E51EC calls it), and runs and draws from the frame tick, which
// every screen has. Closing it clears the flag again.
//
// While the menu is open, and after it closes until the buttons are let go, the game reads no
// pad (func_1501BBB8's pads are emptied), so its buttons do nothing in the game underneath.
//
//     WIREFRAME        the game's 3D triangles drawn as their edges (src/platform/render_debug.hpp)
//     SHOW FPS         the frames the game draws per second, in the corner, on every screen
//     SHOW POSITION    Conker's scene, position and facing, and the camera's yaw and pitch, under
//                      it, during play
//     ENHANCEMENTS     every enhancement that can be switched while the game runs, on and off
//                      (src/enhancements/switches.hpp), until the game is closed
//     TELEPORT         X, Y, Z, facing and camera yaw to move him to, and spots kept in
//                      conker-recomp-data/debug_spots.txt
//     LOAD SNAPSHOT    a list of saves/snapshots/; A starts one (resume_point.cpp)
//     SAVE DEBUG REPORT  a folder in conker-recomp-data/debug_reports/ with report.json (build, scene,
//                      position, facing, camera, enhancements, settings, GPU faults) and a save of the
//                      game as it is, made into a copy, so the player's save is not touched; the first
//                      GPU fault makes one by itself
//     LOAD REPORT      a list of those; A checks one (it may come from anyone) and starts its save,
//                      where and as it was made, on a copy (saves/debug/)
//
// The first page also shows where Conker is and where the camera looks, and opening the menu prints
// them (with the camera's position, look-at point and forward vector). Without Conker (the title)
// the rows that need him say so. Up and down choose,
// A or left and right change (held, a number keeps changing), B goes back, and Back, Start or B on
// the first page close it. The pad is read directly (conker::input::read), not through the game.
// CONKER_DEBUG_MENU=0 turns it off.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "recomp.h"
#include "enhancements/pause_save/resume_point.hpp"
#include "enhancements/status.hpp"
#include "enhancements/switches.hpp"
#include "build_version.h"
#include "json/json.hpp"
#include "platform/gpu_faults.hpp"
#include "platform/input.hpp"
#include "platform/render_debug.hpp"
#include "ultramodern/ultramodern.hpp"

#ifdef _WIN32
#define environ _environ
#else
extern char** environ;
#endif

extern "C" void func_15042D94(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_80010F30(uint8_t* rdram, recomp_context* ctx);

namespace {

using conker::resume::Position;

inline gpr kseg0(uint32_t address) { return static_cast<gpr>(static_cast<int32_t>(address)); }

constexpr uint32_t kMenuHandler = 0x800E0B94;   // D_800E0B94: func_151E51EC's handler, 0 in play
constexpr uint32_t kEntrance = 0x800DD2C0;      // D_800DD2C0: the entrance the scene was entered by
constexpr uint32_t kPadPressed = 0x800BE930;    // D_800BE930: buttons pressed this frame, per pad
constexpr uint32_t kPauseOpen = 0x800BEAC1;     // D_800BEAC1: set when Start opens the pause
constexpr uint32_t kPauseRunning = 0x800BEAC0;  // D_800BEAC0: its copy, made at the end of a frame
constexpr uint32_t kPauseClosing = 0x8008FEEC;  // D_8008FEEC/F0: set to 10 and 5 to close it,
constexpr uint32_t kPauseClose = 0x8008FEF0;    // as A or B on CONT... does
constexpr uint32_t kTextColor = 0x800CBD60;     // D_800CBD60: RGBA given to the next text record
constexpr uint32_t kTextScale = 0x800CBD80;     // D_800CBD80: its scale (func_150432BC)
constexpr uint32_t kText = 0x807FB000;          // free RDRAM (see pause_save.cpp): the format string
constexpr int kTextSize = 0x40;                 // a text record holds 63 characters
constexpr int kOutgoingArg = 0x10;              // the caller's frame: first stack argument
constexpr int kSoundSlot = 0x84;                // func_151DDC20's frame: the sound to play
constexpr int kPads = 0x38;                     // func_1501BBB8's frame: the OSContPads read
constexpr int kPadSize = 6;                     // button (16 bits), stick x, stick y, error
constexpr int kPadCount = 4;

constexpr uint16_t kStartButton = 0x1000;
constexpr uint8_t kShadow = 0x80;               // text flags: a black copy under it, one pixel down and right,
constexpr uint8_t kLeft = kShadow;              // left-aligned at x,
constexpr uint8_t kCentered = kShadow | 1;      // or centered on it
constexpr int kCenterX = 0x92;                  // the middle of the 292-pixel picture
constexpr int32_t kMoveSound = 0x62D;           // the pause lever's
constexpr int32_t kSelectSound = 0x500;
constexpr int32_t kSoundVolume = 0x5DC0;        // as the pause page plays them (0x151DE69C)
constexpr int32_t kSoundPan = 0x40;
constexpr uint32_t kTitleColor = 0xCEC461FF;    // the game's gold
constexpr uint32_t kChosenColor = 0xFFFFFFFF;
constexpr uint32_t kOtherColor = 0x9A9A9AFF;
constexpr float kSmall = 0.8f;                  // rows, lines and the overlay: the title stays full size
constexpr int kTitleY = 40;
constexpr int kFirstRowY = 56;
constexpr int kRowHeight = 10;
constexpr int kLineHeight = 9;                  // the small lines under the rows and in the overlay
constexpr int kListRows = 9;
constexpr int kInfoY = 176;                     // the position and camera lines under the rows
constexpr int kOverlayX = 16;                   // FPS and position in play, top left
constexpr int kOverlayY = 12;
constexpr int kSteps[] = {1, 10, 100, 1000};

using Clock = std::chrono::steady_clock;

enum class Page { Main, Enhancements, Teleport, Spots, Snapshots, Reports };
enum MainRow { kWireframe, kShowFps, kShowPosition, kEnhancements, kTeleport, kLoadSnapshot, kSaveReport, kLoadReport, kMainRows };
enum TeleportRow { kX, kY, kZ, kFacing, kCameraYaw, kStep, kGo, kResetToCurrent, kRemember, kSpotList, kTeleportRows };

bool g_open_requested = false;
bool g_injected = false;
uint16_t g_pad_before = 0;
bool g_active = false;
bool g_holding = false;      // the menu holds the game itself, not the pause (see the top)
bool g_set_pause = false;    // and set the pause flag to do it
bool g_first_frame = false;  // holding, the frame it opened in (see conker_debug_menu_tick)
bool g_pads_held = false;    // the game reads no pad until the buttons are let go
bool g_close_requested = false;
// Text is drawn into the frame's display list, whose size each scene sets (D_800BEBA4 commands,
// from func_15015E80), and func_150417AC stops drawing glyphs once the list is that full. A glyph
// costs about 8 commands, twice with its shadow, so in a scene with a small list (Sloprano's 4002)
// a page of reports runs out: the last row gets its shadow but not itself, and shows black. The
// first time the text runs out while the menu is open, the menu draws without shadows, at half
// the cost, until it closes.
bool g_shadows = true;
Page g_page = Page::Main;
int g_row = 0;           // on the first page
int g_teleport_row = 0;
int g_switch_row = 0;    // on the enhancements page
int g_entry = 0;         // in a list
int g_top = 0;
std::vector<conker::resume::Snapshot> g_snapshots;  // or the reports, on LOAD REPORT
std::vector<Position> g_spots;  // this scene's
std::string g_message;
std::string g_message_detail;  // a smaller line under it
Clock::time_point g_message_until;

bool g_wireframe = false;
bool g_show_fps = false;
bool g_show_position = false;
int g_frames = 0;
Clock::time_point g_fps_since = Clock::now();
int g_fps = 0;

Position g_target;       // the teleport page's X, Y, Z, facing and camera yaw
int g_step = 2;          // index in kSteps

uint16_t g_previous_buttons = 0;

bool debug_menu_enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("CONKER_DEBUG_MENU");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}

void show_message(const std::string& message, const std::string& detail = "") {
    g_message = message;
    g_message_detail = detail;
    g_message_until = Clock::now() + std::chrono::seconds(detail.empty() ? 2 : 4);
}

// The enhancements page's rows: every switch, made once.
const std::vector<conker::switches::Switch>& switches() {
    static const std::vector<conker::switches::Switch> all = conker::switches::all();
    return all;
}

// --- spots ---------------------------------------------------------------------------------------

// conker-recomp-data/debug_spots.txt: one spot a line, "<scene> <x> <y> <z> <yaw>", optionally
// followed by "<facing> <camera yaw> <camera pitch>" (see conker::resume::Position; the pitch is
// only recorded).
std::filesystem::path spots_path() {
    return conker::resume::data_folder() / "debug_spots.txt";
}

std::vector<Position> read_spots() {
    std::vector<Position> spots;
    std::ifstream file(spots_path());
    for (std::string line; std::getline(file, line);) {
        Position spot;
        const int fields = std::sscanf(line.c_str(), "%d %f %f %f %f %f %f %f", &spot.scene, &spot.x, &spot.y, &spot.z,
                                       &spot.yaw, &spot.facing, &spot.camera_yaw, &spot.camera_pitch);
        if (fields >= 5) {
            spot.has_view = fields >= 7;
            if (fields < 8) {
                spot.camera_pitch = NAN;  // not recorded
            }
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
    char line[128];
    if (spot.has_view) {
        std::snprintf(line, sizeof(line), "%d %.2f %.2f %.2f %.2f %.2f %.2f %.2f\n", spot.scene, spot.x, spot.y, spot.z,
                      spot.yaw, spot.facing, spot.camera_yaw, spot.camera_pitch);
    } else {
        std::snprintf(line, sizeof(line), "%d %.2f %.2f %.2f %.2f\n", spot.scene, spot.x, spot.y, spot.z, spot.yaw);
    }
    file << line;
    std::printf("[conker] debug menu: spot kept: %s", line);
}

// "X Y Z  F<facing> Y<camera yaw> P<camera pitch>", as a spot is listed.
std::string describe(const Position& p) {
    char text[kTextSize];
    if (p.has_view && std::isfinite(p.camera_pitch)) {
        std::snprintf(text, sizeof(text), "%.0f %.0f %.0f  F%.0f Y%.0f P%.0f", p.x, p.y, p.z, p.facing, p.camera_yaw,
                      p.camera_pitch);
    } else if (p.has_view) {
        std::snprintf(text, sizeof(text), "%.0f %.0f %.0f  F%.0f Y%.0f", p.x, p.y, p.z, p.facing, p.camera_yaw);
    } else {
        std::snprintf(text, sizeof(text), "%.0f %.0f %.0f", p.x, p.y, p.z);
    }
    return text;
}

// --- drawing -------------------------------------------------------------------------------------

// func_15042D94(x, y, flags, format), as the game queues its HUD text.
void draw_text(uint8_t* rdram, recomp_context* ctx, int x, int y, uint8_t flags, uint32_t color, const std::string& text,
               float scale = kSmall) {
    const size_t length = std::min<size_t>(text.size(), kTextSize - 1);
    for (size_t i = 0; i < length; i++) {
        const char c = text[i] == '%' || text[i] == '#' ? ' ' : text[i];  // format characters
        MEM_B(static_cast<int>(i), kseg0(kText)) = c;
    }
    MEM_B(static_cast<int>(length), kseg0(kText)) = 0;
    const int32_t previous_color = MEM_W(0, kseg0(kTextColor));
    const int32_t previous_scale = MEM_W(0, kseg0(kTextScale));
    MEM_W(0, kseg0(kTextColor)) = static_cast<int32_t>(color);
    uint32_t scale_bits;
    std::memcpy(&scale_bits, &scale, sizeof(scale_bits));
    MEM_W(0, kseg0(kTextScale)) = static_cast<int32_t>(scale_bits);
    recomp_context saved = *ctx;
    ctx->r4 = x;
    ctx->r5 = y;
    ctx->r6 = g_shadows ? flags : flags & ~kShadow;
    ctx->r7 = kseg0(kText);
    MEM_W(kOutgoingArg, ctx->r29) = 0;
    func_15042D94(rdram, ctx);
    *ctx = saved;
    MEM_W(0, kseg0(kTextColor)) = previous_color;
    MEM_W(0, kseg0(kTextScale)) = previous_scale;
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
    draw_text(rdram, ctx, kCenterX, kTitleY, kCentered, kTitleColor, message ? g_message : title, 1.0f);
    if (message && !g_message_detail.empty()) {
        draw_text(rdram, ctx, kCenterX, kTitleY - kLineHeight, kCentered, kOtherColor, g_message_detail);
    }
}

// The rows from `first` on, as many as a list shows (all of them on the first and teleport pages).
void draw_rows(uint8_t* rdram, recomp_context* ctx, const std::vector<std::string>& rows, int chosen, int first = 0,
               int shown = kListRows) {
    const int count = static_cast<int>(rows.size());
    for (int i = first; i < std::min(count, first + shown); i++) {
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
                       std::string("SHOW POSITION  ") + on_off(g_show_position), "ENHANCEMENTS", "TELEPORT",
                       "LOAD SNAPSHOT", "SAVE DEBUG REPORT", "LOAD REPORT"},
                      g_row);
            break;
        case Page::Enhancements: {
            draw_title(rdram, ctx, "ENHANCEMENTS");
            std::vector<std::string> rows;
            for (const conker::switches::Switch& sw : switches()) {
                rows.push_back(std::string(sw.label) + "  " + on_off(sw.on()));
            }
            draw_rows(rdram, ctx, rows, g_switch_row, 0, static_cast<int>(rows.size()));
            break;
        }
        case Page::Teleport:
            draw_title(rdram, ctx, "TELEPORT");
            std::snprintf(text, sizeof(text), "%d", kSteps[g_step]);
            draw_rows(rdram, ctx,
                      {"X  " + std::to_string(static_cast<int>(std::lround(g_target.x))),
                       "Y  " + std::to_string(static_cast<int>(std::lround(g_target.y))),
                       "Z  " + std::to_string(static_cast<int>(std::lround(g_target.z))),
                       "FACING  " + std::to_string(static_cast<int>(std::lround(g_target.facing))),
                       "CAMERA YAW  " + std::to_string(static_cast<int>(std::lround(g_target.camera_yaw))),
                       std::string("STEP  ") + text, "GO", "RESET TO CURRENT", "REMEMBER", "SPOTS  " + std::to_string(g_spots.size())},
                      g_teleport_row, 0, kTeleportRows);
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
        case Page::Snapshots:
        case Page::Reports: {
            const bool reports = g_page == Page::Reports;
            const int count = static_cast<int>(g_snapshots.size());
            const char* name = reports ? "LOAD REPORT" : "LOAD SNAPSHOT";
            if (count > 0) {
                std::snprintf(text, sizeof(text), "%s  %d/%d", name, g_entry + 1, count);
            } else {
                std::snprintf(text, sizeof(text), "%s", name);
            }
            draw_title(rdram, ctx, text);
            if (count == 0 && reports) {
                draw_rows(rdram, ctx, {"NO REPORTS YET", "SAVE ONE ON THE FIRST PAGE"}, -1);
            } else if (count == 0) {
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
        draw_text(rdram, ctx, kCenterX, kInfoY + kLineHeight, kCentered, kOtherColor, camera_line(rdram));
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
    g_shadows = true;
    g_pads_held = true;
    if (g_holding) {
        g_holding = false;
        if (g_set_pause) {
            MEM_B(0, kseg0(kPauseOpen)) = 0;
        }
        return;
    }
    MEM_B(0, kseg0(kPauseClose)) = 5;
    MEM_B(0, kseg0(kPauseClosing)) = 10;
}

// Opens the menu, in the pause that just opened, or (`hold`) holding the game itself.
void open_menu(uint8_t* rdram, bool hold) {
    g_active = true;
    g_holding = hold;
    g_first_frame = hold;
    g_set_pause = false;
    if (hold && MEM_BU(0, kseg0(kPauseOpen)) == 0) {
        MEM_B(0, kseg0(kPauseOpen)) = 1;
        g_set_pause = true;
    }
    g_close_requested = false;
    g_page = Page::Main;
    g_message.clear();
    uint16_t buttons;
    float x, y;
    conker::input::read(&buttons, &x, &y);
    g_previous_buttons = buttons;  // a button held while opening does not count as a press
    std::printf("[conker] debug menu: open (%s, screen %d)\n", hold ? "holding the game" : "in the pause",
                MEM_BU(0, kseg0(kMenuHandler)));
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

// func_80010F30(sound, volume, pan, 0, 0), as the pause page plays its sounds.
void play_sound(uint8_t* rdram, recomp_context* ctx, int32_t sound) {
    recomp_context saved = *ctx;
    ctx->r4 = sound;
    ctx->r5 = kSoundVolume;
    ctx->r6 = kSoundPan;
    ctx->r7 = 0;
    MEM_W(kOutgoingArg, ctx->r29) = 0;
    func_80010F30(rdram, ctx);
    *ctx = saved;
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

// The report SAVE DEBUG REPORT makes (resume_point.cpp adds "save"): everything needed to see what
// the player saw, as JSON.
std::string report_json(uint8_t* rdram) {
    const auto rounded = [](float value) { return std::round(static_cast<double>(value) * 100.0) / 100.0; };
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char when[32];
    std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &local);
    nlohmann::json report = {
        {"format", "conker-recomp-debug-report"},
        {"version", 1},
        {"made", when},
        {"build", CONKER_BUILD_VERSION},
    };
    Position p;
    if (conker::resume::current_position(rdram, &p)) {
        report["scene"] = p.scene;
        report["entrance"] = MEM_BU(0, kseg0(kEntrance));
        report["conker"] = {{"x", rounded(p.x)},           {"y", rounded(p.y)},
                            {"z", rounded(p.z)},           {"facing", rounded(p.facing)},
                            {"angle", rounded(p.yaw)}};
    }
    conker::resume::Camera c;
    if (conker::resume::current_camera(rdram, &c)) {
        report["camera"] = {
            {"eye", {rounded(c.eye[0]), rounded(c.eye[1]), rounded(c.eye[2])}},
            {"look_at", {rounded(c.at[0]), rounded(c.at[1]), rounded(c.at[2])}},
            {"forward", {std::round(c.forward[0] * 1e4) / 1e4, std::round(c.forward[1] * 1e4) / 1e4,
                         std::round(c.forward[2] * 1e4) / 1e4}},
            {"yaw", rounded(c.yaw())},
            {"pitch", rounded(c.pitch())},
        };
    }
    report["enhancements"] = conker::status::enhancements(rdram);
    nlohmann::json settings = conker::status::settings();
    settings["frame_rate"] = g_fps;
    nlohmann::json display = nlohmann::json::object();
    std::ifstream display_file(conker::resume::data_folder() / "display.cfg");
    for (std::string setting; std::getline(display_file, setting);) {
        const size_t equals = setting.find('=');
        if (equals != std::string::npos) {
            display[setting.substr(0, equals)] = setting.substr(equals + 1);
        }
    }
    settings["display_cfg"] = display;
    report["settings"] = settings;
    report["system"] = conker::status::system();
    report["gpu_faults"] = conker::gpu_faults::status();
    // The CONKER_* variables set. A path names its file only: the folders could name the user.
    nlohmann::json environment = nlohmann::json::object();
    for (char** variable = environ; *variable != nullptr; variable++) {
        const std::string entry = *variable;
        if (entry.rfind("CONKER_", 0) == 0) {
            const size_t equals = entry.find('=');
            std::string value = equals == std::string::npos ? "" : entry.substr(equals + 1);
            const size_t slash = value.find_last_of("/\\");
            if (slash != std::string::npos) {
                value = "\u2026/" + value.substr(slash + 1);
            }
            environment[entry.substr(0, equals)] = value;
        }
    }
    report["environment"] = environment;
    return report.dump();
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
        case kEnhancements:
            if (!(pressed & conker::input::kA)) {
                return 0;
            }
            g_page = Page::Enhancements;
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
        case kLoadReport:
            if (!(pressed & conker::input::kA)) {
                return 0;
            }
            g_snapshots = g_row == kLoadReport ? conker::resume::list_reports() : conker::resume::list_snapshots();
            open_list(g_row == kLoadReport ? Page::Reports : Page::Snapshots);
            break;
        case kSaveReport:
            if (!(pressed & conker::input::kA)) {
                return 0;
            }
            show_message(conker::resume::request_report(rdram, report_json(rdram)).empty() ? "SAVING, TRY AGAIN"
                                                                                             : "REPORT SAVED");
            break;
    }
    return kSelectSound;
}

int32_t enhancements_page(int vertical, int horizontal, bool fresh_horizontal, uint16_t pressed) {
    if (pressed & conker::input::kB) {
        g_page = Page::Main;
        return kSelectSound;
    }
    const int count = static_cast<int>(switches().size());
    if (vertical != 0) {
        g_switch_row = (g_switch_row + vertical + count) % count;
        return kMoveSound;
    }
    if (!(pressed & conker::input::kA) && !(horizontal != 0 && fresh_horizontal)) {
        return 0;
    }
    const conker::switches::Switch& sw = switches()[static_cast<size_t>(g_switch_row)];
    sw.set(!sw.on());
    std::printf("[conker] debug menu: %s %s\n", sw.label, sw.on() ? "on" : "off");
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
    float* angle = g_teleport_row == kFacing ? &g_target.facing : g_teleport_row == kCameraYaw ? &g_target.camera_yaw : nullptr;
    if (angle != nullptr && horizontal != 0) {
        const long degrees = (std::lround(*angle) + horizontal * kSteps[g_step]) % 360;
        *angle = static_cast<float>(degrees < 0 ? degrees + 360 : degrees);
        g_target.has_view = true;
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
        case kResetToCurrent:
            if (conker::resume::current_position(rdram, &here)) {
                g_target = here;
            }
            break;
        case kRemember:
            remember_spot(g_target);
            g_spots = scene_spots(g_target.scene);
            if (g_target.has_view) {
                char kept[kTextSize];
                std::snprintf(kept, sizeof(kept), "KEPT  F%.0f Y%.0f P%.0f", g_target.facing, g_target.camera_yaw,
                              g_target.camera_pitch);
                show_message(kept);
            } else {
                show_message("SPOT KEPT");
            }
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
    } else if (g_page == Page::Reports) {
        std::string problem;
        if (conker::resume::load_report(rdram, g_snapshots[g_entry].path, &problem)) {
            close_menu(rdram);
        } else {
            std::transform(problem.begin(), problem.end(), problem.begin(), ::toupper);
            show_message("REPORT REFUSED", problem);  // the log says it too
        }
    } else if (conker::resume::load_snapshot(rdram, g_snapshots[g_entry].path)) {
        close_menu(rdram);
    } else {
        show_message("SAVING, TRY AGAIN");
    }
    return kSelectSound;
}

// One frame of the open menu: reads the pad, does what it asks and draws the page. Returns the
// sound to play; the menu is closed (g_active cleared) when it asked for that.
int32_t menu_frame(uint8_t* rdram, recomp_context* ctx) {
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
        return kSelectSound;
    }

    int32_t sound = 0;
    switch (g_page) {
        case Page::Main:
            sound = main_page(rdram, vertical, horizontal, fresh_horizontal, pressed);
            break;
        case Page::Enhancements:
            sound = enhancements_page(vertical, horizontal, fresh_horizontal, pressed);
            break;
        case Page::Teleport:
            sound = teleport_page(rdram, vertical, horizontal, fresh_horizontal, pressed);
            break;
        case Page::Spots:
        case Page::Snapshots:
        case Page::Reports:
            sound = list_page(rdram, vertical, pressed);
            break;
    }
    if (g_active) {  // not closed by a teleport or a load
        draw_page(rdram, ctx);
    }
    return sound;
}

}  // namespace

// In func_151E51EC, the menu handler, once a frame on every screen (after func_15042D78): counts
// frames, draws the frame rate and position, takes a press of the debug menu button, and runs the
// menu while it holds the game.
extern "C" void conker_debug_menu_tick(uint8_t* rdram, recomp_context* ctx) {
    // After the first GPU fault, a report of where it happened (src/platform/gpu_faults.cpp); asked
    // again next frame while a save is being written.
    if (conker::gpu_faults::take_report_request()) {
        const std::string name = conker::resume::request_report(rdram, report_json(rdram));
        if (name.empty()) {
            conker::gpu_faults::request_report_again();
        } else {
            std::printf("[conker] GPU fault: debug report %s\n", name.c_str());
        }
    }
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
        y += kLineHeight;
    }
    if (g_show_position && !g_active) {
        draw_text(rdram, ctx, kOverlayX, y, kLeft, 0xFFFFFFFF, position_line(rdram));
        draw_text(rdram, ctx, kOverlayX, y + kLineHeight, kLeft, 0xFFFFFFFF, camera_line(rdram));
    }

    if (g_active && MEM_BU(0, kseg0(kPauseOpen)) == 0) {
        g_active = false;  // the pause went away by itself (the game quit to the title, say)
        g_holding = false;
        g_pads_held = true;
    }
    if (conker::input::take_debug_menu_press() && debug_menu_enabled()) {
        if (g_active) {
            g_close_requested = true;
        } else if (MEM_BU(0, kseg0(kMenuHandler)) != 0) {
            open_menu(rdram, true);  // a menu screen: no pause there
        } else if (MEM_BU(0, kseg0(kPauseOpen)) != 0 && MEM_BU(0, kseg0(kPauseRunning)) != 0 &&
                   MEM_BU(0, kseg0(kPauseClosing)) == 0) {
            open_menu(rdram, false);  // the pause menu is up: the page becomes this menu
        } else {
            g_open_requested = true;  // in play: ask for the pause (conker_debug_menu_request)
        }
    }
    // The pause flag has the game copy the last frame as the paused background, a frame after it is
    // set, so the menu is not drawn in the frame it opened in, or its first page would stay in the
    // background of every page.
    if (g_active && g_holding && g_first_frame) {
        g_first_frame = false;
    } else if (g_active && g_holding) {
        const int32_t sound = menu_frame(rdram, ctx);
        if (sound != 0) {
            play_sound(rdram, ctx, sound);
        }
    }
}

// Before func_151E51EC calls the screen's handler and func_151E84B0 its drawing: while the menu
// holds the game, neither runs (the empty func_151DE6CC does, in their place).
extern "C" int conker_debug_menu_holding_game() { return g_active && g_holding ? 1 : 0; }

// At 0x1504286C, where func_150417AC stops drawing a text because the frame's display list is full.
extern "C" void conker_debug_menu_text_full(uint8_t* /*rdram*/, recomp_context* /*ctx*/) {
    if (g_active) {
        g_shadows = false;
    }
}

// In func_1501BBB8 once the pads are read (sp+0x38): while the menu is open, and until every
// button is let go after it closes, the game gets empty pads.
extern "C" void conker_debug_menu_pads(uint8_t* rdram, recomp_context* ctx) {
    if (!g_active && g_pads_held) {
        uint16_t buttons;
        float x, y;
        conker::input::read(&buttons, &x, &y);
        g_pads_held = buttons != 0;
    }
    if (!g_active && !g_pads_held) {
        return;
    }
    for (int pad = 0; pad < kPadCount; pad++) {
        const int offset = kPads + pad * kPadSize;
        MEM_H(offset, ctx->r29) = 0;
        if (g_active) {
            MEM_B(offset + 2, ctx->r29) = 0;  // the stick too, while the menu is up
            MEM_B(offset + 3, ctx->r29) = 0;
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

// At 0x151DDDE0, once func_151DDC20 has looked for Start: take it out again. If the pause opened,
// it is the debug menu's; if it did not (a cutscene, say), the menu holds the game itself.
extern "C" void conker_debug_menu_requested(uint8_t* rdram, recomp_context* /*ctx*/) {
    if (!g_injected) {
        return;
    }
    g_injected = false;
    g_open_requested = false;
    MEM_H(0, kseg0(kPadPressed)) = static_cast<int16_t>(g_pad_before);
    open_menu(rdram, MEM_BU(0, kseg0(kPauseOpen)) == 0);
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

// At 0x151DE00C, where the pause page starts: while the menu is open, run and draw it instead (when
// it is in the pause) and set conker_debug_menu_skip, to skip to the end of the frame.
extern "C" void conker_debug_menu_frame(uint8_t* rdram, recomp_context* ctx) {
    conker_debug_menu_skip = g_active ? 1 : 0;
    if (!g_active || g_holding) {
        return;
    }
    const int32_t sound = menu_frame(rdram, ctx);
    if (sound != 0 || !g_active) {
        MEM_W(kSoundSlot, ctx->r29) = sound;
    }
}

nlohmann::json conker::status::debug_menu() {
    return enhancement("debug_menu", "Debug menu", debug_menu_enabled(), "CONKER_DEBUG_MENU",
                       {{"wireframe", g_wireframe}, {"show_fps", g_show_fps}, {"show_position", g_show_position}});
}
