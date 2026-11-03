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
//     DEBUG VIEW       a page of what is drawn to look into the game:
//       WIREFRAME      the game's 3D triangles drawn as their edges (src/platform/render_debug.hpp)
//       SHOW FPS       the frames the game draws per second, in the corner, on every screen
//       SHOW POSITION  Conker's scene, position and facing, and the camera's yaw and pitch, under
//                      it, during play
//       INSPECT        the mouse points at something to name it (an actor and its model, an
//                      attachment, a prop, the function that drew it), the wheel looks behind it, a
//                      left click picks it out, highlighted and reported in the console, a right
//                      click lets it go (src/enhancements/object_inspector)
//     ENHANCEMENTS     every enhancement that can be switched while the game runs, on and off
//                      (src/enhancements/switches.hpp), until the game is closed
//     CONTROLS         for each controller: INVERT AIM (consistent controls' aim, pushed up to aim
//                      down), INVERT MOUSE AIM, and the pad's button for each of the game's buttons:
//                      A, then a press on that controller's pad (or controller 1's keyboard, which
//                      is listed on its own), maps it (taking it off any other),
//                      R adds another, Z clears it, L puts that button's back; RESET BUTTONS puts
//                      them all back. The hints name the buttons as controller 1's pad has them. Kept in
//                      conker-recomp-data/input.cfg (src/platform/input.hpp's mapping). The menu
//                      itself reads controller 1 as the default mapping has it, so it can always be
//                      steered back
//     MODS             the mods in conker-recomp-data/mods, on and off (kept in mods.json; a new one
//                      starts off, and the page says to trust where a mod came from); mods load
//                      when the game starts, so after a change RESTART TO APPLY starts it again
//     TELEPORT         X, Y, Z, facing and camera yaw to move him to, and spots kept in
//                      conker-recomp-data/debug_spots.txt
//     LOAD SNAPSHOT    a list of saves/snapshots/; A starts one (resume_point.cpp)
//     SAVE DEBUG REPORT  a folder in conker-recomp-data/debug_reports/ with report.json (build, scene,
//                      position, facing, camera, enhancements, settings, GPU faults) and a save of the
//                      game as it is, made into a copy, so the player's save is not touched; the first
//                      GPU fault makes one by itself
//     LOAD REPORT      a list of those; A checks one (it may come from anyone) and starts its save,
//                      where and as it was made, on a copy (saves/debug/); R starts it at its
//                      checkpoint instead, as the game itself would
//
// The first page also shows where Conker is and where the camera looks, and opening the menu prints
// them (with the camera's position, look-at point and forward vector). Without Conker (the title)
// the rows that need him say so. Up and down choose,
// A or left and right change (held, a number keeps changing), B goes back, and Back, Start or B on
// the first page close it. The pad is read directly (conker::input::read_menu), not through the game.
// CONKER_DEBUG_MENU=0 turns it off.
#include <algorithm>
#include <cctype>
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
#include "librecomp/mods.hpp"
#include "enhancements/debug_menu/debug_menu.hpp"
#include "enhancements/object_inspector/object_inspector.hpp"
#include "enhancements/pause_save/resume_point.hpp"
#include "enhancements/status.hpp"
#include "enhancements/switches.hpp"
#include "build_version.h"
#include "json/json.hpp"
#include "platform/gpu_faults.hpp"
#include "platform/input.hpp"
#include "net/lobby_ui.hpp"
#include "platform/render_debug.hpp"
#include "runtime/frame_input.hpp"
#include "net/net_play.hpp"
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
constexpr uint32_t kTextPlace = 0x800CBD70;     // D_800CBD70/72: x, y, and D_800CBD74 the flags, which
constexpr uint32_t kTextFlags = 0x800CBD74;     // func_15042D94 leaves set for the game's next text
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
constexpr uint32_t kWarningColor = 0xFF8060FF;
constexpr float kSmall = 0.8f;                  // rows, lines and the overlay: the title stays full size
constexpr int kTitleY = 47;   // under the sign at the top of a menu screen (GAME1, OPTIONS)
constexpr int kFirstRowY = 63;
constexpr int kRowHeight = 10;
constexpr int kLineHeight = 9;                  // the small lines under the rows and in the overlay
constexpr int kListRows = 9;
constexpr int kInfoY = 176;                     // the position and camera lines under the rows
constexpr int kOverlayX = 16;                   // FPS and position in play, top left
constexpr int kOverlayY = 12;
// The title's doors (screen 3, func_151DE8F0), found by watching memory while scripted presses
// turned the doors and went into MULTI: the selected door, u8 at D_800E0B1C, is 5 GAME1, 0 MULTI,
// 1 OPTIONS, 2 CHAPTERS, and 4 once GAME1's or MULTI's door is opened. Behind MULTI's door (the
// barrel with the skull) f32 D_800E0AF8 goes from 0 to 1 as the barrel turns to the modes, and
// back as it turns away; u8 D_800E0B30 is 4 on the join screen and 0 before it.
constexpr uint32_t kTitleDoor = 0x800E0B1C;
constexpr uint32_t kBarrelTurn = 0x800E0AF8;
constexpr uint32_t kMultiJoin = 0x800E0B30;
constexpr int kTitleScreen = 3;
constexpr uint8_t kMultiDoor = 0;
constexpr uint8_t kDoorOpened = 4;
constexpr int kOnlinePromptY = 196;  // under the door
constexpr int kSteps[] = {1, 10, 100, 1000};

using Clock = std::chrono::steady_clock;

enum class Page { Main, DebugView, Enhancements, Controls, Mods, Teleport, Spots, Snapshots, Reports };
enum MainRow { kDebugView, kEnhancements, kControls, kMods, kTeleport, kLoadSnapshot, kSaveReport, kLoadReport, kMainRows };
enum DebugViewRow { kWireframe, kShowFps, kShowPosition, kInspect, kDebugViewRows };
// The controls page's rows: these, then a row for each of the mapped buttons (for the keyboard, then
// its stick, walk and zoom keys), then RESET BUTTONS.
enum ControlsRow { kController, kInvertAim, kMouseAim, kInvertMouseAim, kFirstMappedRow };
// MOUSE AIM's speeds, percent: how fast the captured mouse aims (frame_input::mouse_speed).
constexpr int kMouseSpeeds[] = {25, 50, 75, 100, 125, 150, 175, 200, 250, 300, 400};
int reset_buttons_row(bool keys) {
    return kFirstMappedRow + (keys ? conker::input::kKeyRows : conker::input::kMappedButtonCount);
}
int controls_rows(bool keys) { return reset_buttons_row(keys) + 1; }
constexpr auto kListenFor = std::chrono::seconds(5);
// The controls page's controllers: controller 1's pad, its keyboard, then controllers 2 to 4.
constexpr int kKeyboardDevice = 1;
constexpr int kDevices = conker::input::kPorts + 1;
int device_port(int device) { return device <= kKeyboardDevice ? 0 : device - 1; }
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
int g_view_row = 0;      // on the debug view page
int g_teleport_row = 0;
int g_switch_row = 0;    // on the enhancements page, a list like the reports'
int g_switch_top = 0;    // and its first row shown
int g_mod_row = 0;       // on the mods page, a list like the reports'
int g_mod_top = 0;       // and its first row shown
int g_controls_row = 0;  // on the controls page, a list like the reports'
int g_controls_top = 0;  // and its first row shown
int g_controls_device = 0;  // the controller it shows: kKeyboardDevice is controller 1's keyboard
int g_listening = -1;    // the mapped button a press on the pad is waiting for, or -1
bool g_controls_requested = false;  // the OPTIONS page's CONTROLS row asked for the page
bool g_controls_only = false;       // the menu shows the controls page alone, over OPTIONS
bool g_listen_adds = false;  // and whether it adds to the button's controls (R) or replaces them (A)
Clock::time_point g_listen_until;
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
    // Off online: it pauses the game and writes its memory on one machine only.
    static const bool enabled = [] {
        const char* value = std::getenv("CONKER_DEBUG_MENU");
        return (value == nullptr || value[0] != '0') && !conker::net::active();
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

// --- mods ----------------------------------------------------------------------------------------

constexpr char kModGameId[] = "conker";  // the game's mod_game_id (src/runtime/game.cpp)

struct Mod {
    std::string id;
    std::string version;
    std::string label;        // the display name, upper case for the game's font
    std::string description;  // the short description, the same
    bool loaded;              // on when the game started, so its code is running
};

std::string upper(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::toupper(c); });
    return text;
}

bool mod_on(const std::string& id) {
    return recomp::mods::is_mod_enabled(id) || recomp::mods::is_mod_auto_enabled(id);
}

// The mods page's rows: the mods librecomp found when the game started (it scans the folder then).
// Whether each was on is taken the first time, before the page can change anything.
const std::vector<Mod>& mods() {
    static const std::vector<Mod> all = [] {
        std::vector<Mod> list;
        for (const recomp::mods::ModDetails& details : recomp::mods::get_all_mod_details(kModGameId)) {
            const std::string& description = details.short_description.empty() ? details.description : details.short_description;
            list.push_back({details.mod_id, details.version.to_string(), upper(details.display_name), upper(description),
                            mod_on(details.mod_id)});
        }
        return list;
    }();
    return all;
}

// A mod switched since the game started, which only a restart applies.
bool mods_changed() {
    return std::any_of(mods().begin(), mods().end(), [](const Mod& mod) { return mod_on(mod.id) != mod.loaded; });
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

// func_15042D94(x, y, flags, format), as the game queues its HUD text. It leaves x, y and the flags
// set for the game's next text, so they are put back: the OPTIONS rows lost their centering.
void draw_text(uint8_t* rdram, recomp_context* ctx, int x, int y, uint8_t flags, uint32_t color, const std::string& text,
               float scale = kSmall) {
    // The text is a format string: % is written %%, and # (a format character too) as a space.
    int length = 0;
    for (const char c : text) {
        const int room = c == '%' ? 2 : 1;
        if (length + room > kTextSize - 1) {
            break;
        }
        MEM_B(length++, kseg0(kText)) = c == '#' ? ' ' : c;
        if (c == '%') {
            MEM_B(length++, kseg0(kText)) = '%';
        }
    }
    MEM_B(length, kseg0(kText)) = 0;
    const int32_t previous_color = MEM_W(0, kseg0(kTextColor));
    const int32_t previous_scale = MEM_W(0, kseg0(kTextScale));
    const int32_t previous_place = MEM_W(0, kseg0(kTextPlace));
    const uint8_t previous_flags = MEM_BU(0, kseg0(kTextFlags));
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
    MEM_W(0, kseg0(kTextPlace)) = previous_place;  // the OPTIONS rows are centered by the flags left here
    MEM_B(0, kseg0(kTextFlags)) = previous_flags;
}

const char* on_off(bool on) { return on ? "ON" : "OFF"; }

// `text` broken at spaces into lines short enough to fit across the picture in the small text (the
// 39-letter warning on the mods page spans about 60% of it), at most `max_lines`, the last cut with
// "..." if the rest does not fit.
std::vector<std::string> wrap_lines(const std::string& text, size_t max_lines) {
    constexpr size_t kLineLetters = 38;
    std::vector<std::string> lines;
    std::string line;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find(' ', at);
        if (end == std::string::npos) end = text.size();
        std::string word = text.substr(at, end - at);
        at = end + 1;
        if (word.size() > kLineLetters) word = word.substr(0, kLineLetters);
        if (!line.empty() && line.size() + 1 + word.size() > kLineLetters) {
            lines.push_back(line);
            line.clear();
        }
        line += (line.empty() ? "" : " ") + word;
    }
    if (!line.empty()) lines.push_back(line);
    if (lines.size() > max_lines) {
        lines.resize(max_lines);
        std::string& last = lines.back();
        last = last.substr(0, std::min(last.size(), kLineLetters - 3)) + "...";
    }
    return lines;
}

// The chapter a scene belongs to, as the CHAPTERS menu names it, or "SCENE n" for the rest. Each
// sub-chapter there (bank 8, one record per chapter) starts an event, and the event table at
// D_80087430 (0x14-byte records: name, id, scene +8, entrance +0xA) gives that event's scene; the
// scenes no sub-chapter starts in are placed by the named events the table puts in them. Scene 6
// is Windy's, though IT'S WAR also starts there.
std::string scene_label(int scene) {
    switch (scene) {
        case 0x29:
            return "HUNGOVER";
        case 0x00: case 0x06: case 0x2C:
            return "WINDY";
        case 0x07: case 0x0A: case 0x0B: case 0x12: case 0x17:
            return "BATS TOWER";
        case 0x01: case 0x0C: case 0x10:
            return "BARN BOYS";
        case 0x35: case 0x37:
            return "SLOPRANO";
        case 0x02: case 0x14: case 0x31: case 0x32: case 0x43:
            return "UGA BUGA";
        case 0x3C: case 0x41:
            return "SPOOKY";
        case 0x04: case 0x0E: case 0x13: case 0x1B: case 0x1C: case 0x1E: case 0x23: case 0x26: case 0x28: case 0x2F:
            return "IT'S WAR";
        case 0x36: case 0x38: case 0x39: case 0x3A: case 0x3B: case 0x40:
            return "HEIST";
        case 0x1A: case 0x24: case 0x2B: case 0x2D: case 0x30: case 0x33: case 0x34: case 0x3F:
            return "MULTIPLAYER";
        case 0x18:
            return "THRONE ROOM";
        case 0x1D:
            return "TITLE";
        case 0x21:
            return "INTRO";
        case 0x22:
            return "GREGG";
        case 0x25:
            return "LOGOS";
        default:
            return "SCENE " + std::to_string(scene);
    }
}

// The CHAPTERS menu's sub-chapters, in its order (its strings, archive entry [0x1C, 0, 2]): a
// chapter's record in bank 8 names each sub-chapter's event, which unlocks it once its bit is set in
// the event flags (func_1509D08C), and the event table gives the scene it starts in.
struct SubChapter {
    const char* chapter;
    const char* name;
    int event;
    int scene;
};

constexpr SubChapter kSubChapters[] = {
    {"HUNGOVER", "SCAREDY BIRDY", 0, 0x29},
    {"HUNGOVER", "PAN HANDLED", 4, 0x29},
    {"HUNGOVER", "GARGOYLE", 6, 0x29},
    {"WINDY", "MRS BEE", 10, 0x06},
    {"WINDY", "POO CABIN", 117, 0x2C},
    {"WINDY", "PRUNED", 132, 0x00},
    {"WINDY", "YEE HAA!", 133, 0x00},
    {"WINDY", "SEWAGE SUCKS", 120, 0x2C},
    {"WINDY", "GREAT BALLS OF POO", 28, 0x06},
    {"WINDY", "WASPS' REVENGE", 11, 0x06},
    {"WINDY", "MR BARREL", 39, 0x06},
    {"BATS TOWER", "MRS CATFISH", 69, 0x07},
    {"BATS TOWER", "BARRY'S MATE", 85, 0x0B},
    {"BATS TOWER", "COGS' REVENGE", 81, 0x0B},
    {"BATS TOWER", "THE COMBINATION", 70, 0x07},
    {"BATS TOWER", "BLAST DOORS", 75, 0x12},
    {"BATS TOWER", "CLANG'S LAIR", 77, 0x12},
    {"BATS TOWER", "PISSTASTIC", 134, 0x0A},
    {"BATS TOWER", "BRASS MONKEYS", 135, 0x0A},
    {"BATS TOWER", "BULLFISH'S REVENGE", 73, 0x07},
    {"BARN BOYS", "MARVIN", 45, 0x0C},
    {"BARN BOYS", "MAD PITCHFORK", 60, 0x01},
    {"BARN BOYS", "SUNNY DAYS", 54, 0x0C},
    {"BARN BOYS", "BARRY + CO", 61, 0x01},
    {"BARN BOYS", "BUFF YOU", 65, 0x01},
    {"BARN BOYS", "HAYBOT WARS", 67, 0x10},
    {"BARN BOYS", "FRYING TONIGHT", 68, 0x10},
    {"BARN BOYS", "SLAM DUNK", 59, 0x0C},
    {"SLOPRANO", "CORN OFF THE COB", 122, 0x35},
    {"SLOPRANO", "SWEET MELODY", 123, 0x35},
    {"SLOPRANO", "U-BEND BLUES", 124, 0x35},
    {"SLOPRANO", "THE BLUFF", 126, 0x37},
    {"UGA BUGA", "DRUNKEN GITS", 87, 0x02},
    {"UGA BUGA", "SACRIFICE", 102, 0x43},
    {"UGA BUGA", "PHLEGM", 94, 0x02},
    {"UGA BUGA", "WORSHIP", 96, 0x02},
    {"UGA BUGA", "ROCK SOLID", 107, 0x31},
    {"UGA BUGA", "BOMB RUN", 98, 0x02},
    {"UGA BUGA", "MUGGED", 130, 0x32},
    {"UGA BUGA", "RAPTOR FOOD", 113, 0x14},
    {"UGA BUGA", "BUGA THE KNUT", 114, 0x14},
    {"SPOOKY", "MR DEATH", 166, 0x41},
    {"SPOOKY", "COUNT BATULA", 169, 0x3C},
    {"SPOOKY", "ZOMBIES", 171, 0x3C},
    {"SPOOKY", "MR BARREL", 173, 0x3C},
    {"IT'S WAR", "IT'S WAR", 197, 0x06},
    {"IT'S WAR", "POWER'S OFF", 158, 0x13},
    {"IT'S WAR", "TNT", 159, 0x13},
    {"IT'S WAR", "THE ASSAULT", 138, 0x04},
    {"IT'S WAR", "SOLE SURVIVOR", 139, 0x04},
    {"IT'S WAR", "CASUALTY DEPT.", 164, 0x2F},
    {"IT'S WAR", "SAVING PRIVATE RODENT", 147, 0x1C},
    {"IT'S WAR", "CHEMICAL WARFARE", 154, 0x0E},
    {"IT'S WAR", "THE TOWER", 155, 0x0E},
    {"IT'S WAR", "LITTLE GIRL", 161, 0x23},
    {"IT'S WAR", "THE EXPERIMENT", 162, 0x23},
    {"IT'S WAR", "COUNTDOWN", 163, 0x23},
    {"IT'S WAR", "PEACE AT LAST!", 141, 0x04},
    {"HEIST", "THE WINDMILL'S DEAD", 42, 0x3B},
    {"HEIST", "ENTER THE VERTEX", 177, 0x36},
    {"HEIST", "THE VAULT", 182, 0x40},
    {"HEIST", "END CUTSCENE", 199, 0x40},
};

// What a saved game was doing: of the sub-chapters its event flags have unlocked, the last that
// starts in `scene`, else the last of the scene's chapter, else (a scene in no chapter) the last of
// all; the scene's label when none is unlocked or the flags could not be read.
std::string saved_label(int scene, const std::vector<uint8_t>& events) {
    const std::string chapter = scene_label(scene);
    const SubChapter* in_scene = nullptr;
    const SubChapter* in_chapter = nullptr;
    const SubChapter* last = nullptr;
    for (const SubChapter& sub : kSubChapters) {
        const size_t byte = sub.event >> 3;
        if (byte >= events.size() || !(events[byte] >> (sub.event & 7) & 1)) {
            continue;
        }
        last = &sub;
        in_scene = sub.scene == scene ? &sub : in_scene;
        in_chapter = chapter == sub.chapter ? &sub : in_chapter;
    }
    const SubChapter* chosen = in_scene != nullptr ? in_scene : in_chapter;
    if (chosen == nullptr && chapter.rfind("SCENE ", 0) == 0) {
        chosen = last;
    }
    return chosen != nullptr ? chosen->name : chapter;
}

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

// The menu's A, R, Z and L as controller 1 has them (read_menu: the default mapping), for the hints.
struct MenuButtonNames {
    const char* a;
    const char* r;
    const char* z;
    const char* l;
};

MenuButtonNames menu_button_names() {
    switch (conker::input::controller_kind()) {
        case conker::input::PadKind::PlayStation:
            return {"CROSS", "R1", "R2", "L1"};
        case conker::input::PadKind::None:
            return {"SPACE", "F", "C", "Q"};
        default:
            return {"A", "RB", "RT", "LB"};
    }
}

// What a controller's pad is, without its name (often the owner's).
std::string pad_line(int port) {
    if (port != 0 && !conker::input::connected(port)) {
        return "NOT CONNECTED";
    }
    switch (conker::input::controller_kind(port)) {
        case conker::input::PadKind::PlayStation:
            return "PLAYSTATION PAD";
        case conker::input::PadKind::Xbox:
            return "XBOX PAD";
        case conker::input::PadKind::Other:
            return "PAD";
        default:
            return "KEYBOARD ONLY, NO PAD";
    }
}

void draw_controls_page(uint8_t* rdram, recomp_context* ctx) {
    namespace input = conker::input;
    const int port = device_port(g_controls_device);
    const bool keys = g_controls_device == kKeyboardDevice;
    if (g_listening >= 0) {
        draw_title(rdram, ctx, std::string(keys ? "PRESS A KEY FOR " : "PRESS A BUTTON FOR ") +
                                   (keys ? input::key_row_label(g_listening) : input::kMappedButtons[g_listening].label));
    } else {
        // Where in the list, as the other lists show it: it scrolls.
        draw_title(rdram, ctx, "CONTROLS  " + std::to_string(g_controls_row + 1) + "/" + std::to_string(controls_rows(keys)));
    }
    std::vector<std::string> rows = {
        "CONTROLLER  " + std::to_string(port + 1) + (port != 0 ? "" : keys ? "  KEYBOARD" : "  PAD"),
        std::string("INVERT AIM  ") + on_off(conker::frame_input::invert_aim(port)),
        "MOUSE AIM  " + std::to_string(conker::frame_input::mouse_speed()) + "%",
        std::string("INVERT MOUSE AIM  ") + on_off(conker::frame_input::invert_mouse_aim()),
    };
    for (int i = 0; i < (keys ? input::kKeyRows : input::kMappedButtonCount); i++) {
        const std::string controls = i == g_listening                  ? "..."
                                     : keys                            ? input::key_names(input::key_mapping(i))
                                                                       : input::control_names(input::mapping(port, i), port);
        rows.push_back(std::string(keys ? input::key_row_label(i) : input::kMappedButtons[i].label) + "  " + controls);
    }
    const bool changed = keys ? !input::key_mapping_is_default() : !input::mapping_is_default(port);
    rows.push_back(changed ? "RESET BUTTONS  CHANGED" : "RESET BUTTONS");
    draw_rows(rdram, ctx, rows, g_controls_row, g_controls_top);

    std::string info;
    std::string more;
    uint32_t more_color = kOtherColor;
    if (g_listening >= 0) {
        const auto left = std::chrono::duration_cast<std::chrono::seconds>(g_listen_until - Clock::now()).count() + 1;
        info = keys ? "ON THE KEYBOARD" : "ON CONTROLLER " + std::to_string(port + 1) + "'S PAD";
        more = (keys ? "THE MENU KEY CANCELS  OR WAIT " : "BACK CANCELS  OR WAIT ") + std::to_string(std::max<long long>(left, 1));
    } else if (g_controls_row == kController) {
        info = keys ? "EVERY KEY BUT ESC AND THE MENU KEY" : pad_line(port);
        more = "LEFT AND RIGHT CHOOSE";
    } else if (g_controls_row == kMouseAim) {
        info = "HOW FAST CONTROLLER 1'S MOUSE AIMS";
        more = "LEFT AND RIGHT CHANGE";
    } else if (g_controls_row == kInvertAim || g_controls_row == kInvertMouseAim) {
        info = g_controls_row == kInvertAim ? "THE STICK PUSHED UP AIMS DOWN AS ON THE N64" : "CONTROLLER 1'S MOUSE PUSHED UP AIMS DOWN";
        if (!conker::switches::consistent_controls().on()) {
            more = "NEEDS CONSISTENT CONTROLS ON";
            more_color = kWarningColor;
        }
    } else if (g_controls_row == reset_buttons_row(keys)) {
        info = keys ? "THE KEYBOARD'S KEYS AS THEY WERE" : "CONTROLLER " + std::to_string(port + 1) + "'S BUTTONS AS THEY WERE";
    } else {
        const MenuButtonNames names = menu_button_names();
        info = std::string(names.a) + " CHANGES   " + names.r + " ADDS ANOTHER";
        more = std::string(names.z) + " CLEARS   " + names.l + " PUTS IT BACK";
    }
    draw_text(rdram, ctx, kCenterX, kInfoY, kCentered, kOtherColor, info);
    draw_text(rdram, ctx, kCenterX, kInfoY + kLineHeight, kCentered, more_color, more);
}

void draw_page(uint8_t* rdram, recomp_context* ctx) {
    char text[kTextSize];
    switch (g_page) {
        case Page::Main:
            draw_title(rdram, ctx, "DEBUG MENU");
            draw_rows(rdram, ctx,
                      {"DEBUG VIEW", "ENHANCEMENTS", "CONTROLS", "MODS", "TELEPORT", "LOAD SNAPSHOT", "SAVE DEBUG REPORT",
                       "LOAD REPORT"},
                      g_row, 0, kMainRows);
            break;
        case Page::DebugView:
            draw_title(rdram, ctx, "DEBUG VIEW");
            draw_rows(rdram, ctx,
                      {std::string("WIREFRAME  ") + on_off(g_wireframe), std::string("SHOW FPS  ") + on_off(g_show_fps),
                       std::string("SHOW POSITION  ") + on_off(g_show_position),
                       std::string("INSPECT  ") + on_off(conker::inspector::active())},
                      g_view_row, 0, kDebugViewRows);
            break;
        case Page::Enhancements: {
            std::snprintf(text, sizeof(text), "ENHANCEMENTS  %d/%d", g_switch_row + 1,
                          static_cast<int>(switches().size()));
            draw_title(rdram, ctx, text);
            std::vector<std::string> rows;
            for (const conker::switches::Switch& sw : switches()) {
                rows.push_back(std::string(sw.label) + "  " + on_off(sw.on()));
            }
            draw_rows(rdram, ctx, rows, g_switch_row, g_switch_top);
            break;
        }
        case Page::Controls:
            draw_controls_page(rdram, ctx);
            break;
        case Page::Mods: {
            const int count = static_cast<int>(mods().size());
            // A mod is code someone else wrote, running in the game: always said on this page.
            draw_text(rdram, ctx, kCenterX, kInfoY + 2 * kLineHeight, kCentered, kWarningColor,
                      "ONLY TURN ON MODS FROM PEOPLE YOU TRUST");
            if (count > 0 && g_mod_row < count) {
                std::snprintf(text, sizeof(text), "MODS  %d/%d", g_mod_row + 1, count);
            } else {
                std::snprintf(text, sizeof(text), "MODS");
            }
            draw_title(rdram, ctx, text);
            if (count == 0) {
                draw_rows(rdram, ctx, {"NO MODS INSTALLED", "PUT .NRM FILES IN", "CONKER-RECOMP-DATA/MODS"}, -1);
                break;
            }
            std::vector<std::string> rows;
            for (const Mod& mod : mods()) {
                rows.push_back(mod.label + "  " + on_off(mod_on(mod.id)));
            }
            const bool changed = mods_changed();
            if (changed) {
                rows.push_back("RESTART TO APPLY");
            }
            draw_rows(rdram, ctx, rows, g_mod_row, g_mod_top);
            const bool restart_row = g_mod_row == static_cast<int>(mods().size());
            if (restart_row) {
                draw_text(rdram, ctx, kCenterX, kInfoY, kCentered, kOtherColor, "STARTS THE GAME AGAIN");
                draw_text(rdram, ctx, kCenterX, kInfoY + kLineHeight, kCentered, kOtherColor, "WHAT IS NOT SAVED IS LOST");
            } else {
                // Wrapped, ending on the line where one line would be, so the lines below stay put.
                const std::vector<std::string> lines = wrap_lines(mods()[g_mod_row].description, 3);
                const int first_y = kInfoY - static_cast<int>(lines.size() - 1) * kLineHeight;
                for (size_t i = 0; i < lines.size(); i++) {
                    draw_text(rdram, ctx, kCenterX, first_y + static_cast<int>(i) * kLineHeight, kCentered, kOtherColor, lines[i]);
                }
                if (changed) {
                    draw_text(rdram, ctx, kCenterX, kInfoY + kLineHeight, kCentered, kOtherColor,
                              "CHANGES APPLY WHEN THE GAME STARTS AGAIN");
                }
            }
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
                rows.push_back(saved_label(snapshot.scene, snapshot.events) + "  " + snapshot.when);
            }
            draw_rows(rdram, ctx, rows, g_entry, g_top);
            if (reports && count > 0) {
                draw_text(rdram, ctx, kCenterX, kInfoY, kCentered, kOtherColor, "A WHERE IT WAS MADE   R AT ITS CHECKPOINT");
            }
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
    if (g_listening >= 0) {
        g_listening = -1;
        conker::input::stop_listening();
    }
    g_controls_only = false;
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

// Opens the menu, in the pause that just opened, or (`hold`) holding the game itself: the screen's
// own handler stopped and, with `pause`, the world too.
void open_menu(uint8_t* rdram, bool hold, bool pause = true) {
    g_active = true;
    g_holding = hold;
    g_first_frame = hold && pause;  // a frame for the paused picture
    g_set_pause = false;
    if (hold && pause && MEM_BU(0, kseg0(kPauseOpen)) == 0) {
        MEM_B(0, kseg0(kPauseOpen)) = 1;
        g_set_pause = true;
    }
    g_close_requested = false;
    g_page = Page::Main;
    g_message.clear();
    uint16_t buttons;
    float x, y;
    conker::input::read_menu(&buttons, &x, &y);
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
    // Every installed mod, and whether it was on as the game started (what a change on the MODS page
    // applies only after a restart), so a report from a modded game says so.
    nlohmann::json installed = nlohmann::json::array();
    for (const Mod& mod : mods()) {
        installed.push_back({{"id", mod.id}, {"version", mod.version}, {"on", mod.loaded}});
    }
    report["mods"] = installed;
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
        case kDebugView:
            if (!(pressed & conker::input::kA)) {
                return 0;
            }
            g_page = Page::DebugView;
            g_view_row = 0;
            break;
        case kEnhancements:
            if (!(pressed & conker::input::kA)) {
                return 0;
            }
            g_page = Page::Enhancements;
            g_switch_row = 0;
            g_switch_top = 0;
            break;
        case kControls:
            if (!(pressed & conker::input::kA)) {
                return 0;
            }
            g_page = Page::Controls;
            g_controls_row = 0;
            g_controls_top = 0;
            break;
        case kMods:
            if (!(pressed & conker::input::kA)) {
                return 0;
            }
            g_page = Page::Mods;
            g_mod_row = 0;
            g_mod_top = 0;
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

int32_t debug_view_page(int vertical, int horizontal, bool fresh_horizontal, uint16_t pressed) {
    if (pressed & conker::input::kB) {
        g_page = Page::Main;
        return kSelectSound;
    }
    if (vertical != 0) {
        g_view_row = (g_view_row + vertical + kDebugViewRows) % kDebugViewRows;
        return kMoveSound;
    }
    if (!(pressed & conker::input::kA) && !(horizontal != 0 && fresh_horizontal)) {
        return 0;
    }
    switch (g_view_row) {
        case kWireframe:
            set_wireframe(!g_wireframe);
            break;
        case kShowFps:
            g_show_fps = !g_show_fps;
            break;
        case kShowPosition:
            g_show_position = !g_show_position;
            break;
        case kInspect:
            conker::inspector::set_active(!conker::inspector::active());
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
    // A list like the reports': it stops at either end and scrolls to keep the chosen row in view.
    if (vertical != 0) {
        g_switch_row = std::clamp(g_switch_row + vertical, 0, count - 1);
        g_switch_top = std::clamp(g_switch_top, g_switch_row - kListRows + 1, g_switch_row);
        g_switch_top = std::clamp(g_switch_top, 0, std::max(count - kListRows, 0));
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

// The controls page. While a press on the pad is awaited, that is all it does.
int32_t controls_page(int vertical, int horizontal, bool fresh_horizontal, uint16_t pressed) {
    namespace input = conker::input;
    const int port = device_port(g_controls_device);
    const bool keys = g_controls_device == kKeyboardDevice;
    if (g_listening >= 0) {
        if (keys) {
            if (const int heard = input::take_heard_key(); heard != 0) {
                std::vector<int> mapped = g_listen_adds ? input::key_mapping(g_listening) : std::vector<int>();
                mapped.push_back(heard);
                input::set_key_mapping(g_listening, mapped);
                g_listening = -1;
                return kSelectSound;
            }
        } else if (const input::Controls heard = input::take_heard(); heard != 0) {
            const input::Controls controls = g_listen_adds ? input::mapping(port, g_listening) | heard : heard;
            input::set_mapping(port, g_listening, controls);
            g_listening = -1;
            return kSelectSound;
        }
        if (Clock::now() >= g_listen_until) {
            input::stop_listening();
            g_listening = -1;
            show_message("NOTHING PRESSED");
        }
        return 0;
    }
    if (pressed & input::kB) {
        if (g_controls_only) {
            g_close_requested = true;  // back to the OPTIONS page it came from
        } else {
            g_page = Page::Main;
        }
        return kSelectSound;
    }
    // A list like the reports': it stops at either end and scrolls to keep the chosen row in view.
    if (vertical != 0) {
        g_controls_row = std::clamp(g_controls_row + vertical, 0, controls_rows(keys) - 1);
        g_controls_top = std::clamp(g_controls_top, g_controls_row - kListRows + 1, g_controls_row);
        g_controls_top = std::clamp(g_controls_top, 0, std::max(controls_rows(keys) - kListRows, 0));
        return kMoveSound;
    }
    const bool a = (pressed & input::kA) != 0;
    const bool toggle = a || (horizontal != 0 && fresh_horizontal);
    if (g_controls_row == reset_buttons_row(keys)) {
        if (!a) {
            return 0;
        }
        if (keys) {
            input::reset_key_mapping();
        } else {
            input::reset_mapping(port);
        }
        show_message(keys ? "KEYS RESET" : "BUTTONS RESET");
        return kSelectSound;
    }
    switch (g_controls_row) {
        case kController:
            if (!toggle) {
                return 0;
            }
            g_controls_device = (g_controls_device + (horizontal != 0 ? horizontal : 1) + kDevices) % kDevices;
            return kMoveSound;
        case kInvertAim:
            if (!toggle) {
                return 0;
            }
            conker::frame_input::set_invert_aim(port, !conker::frame_input::invert_aim(port));
            return kSelectSound;
        case kInvertMouseAim:
            if (!toggle) {
                return 0;
            }
            conker::frame_input::set_invert_mouse_aim(!conker::frame_input::invert_mouse_aim());
            return kSelectSound;
        case kMouseAim: {
            // Left slower, right (or A) faster, stopping at either end.
            if (horizontal == 0 && !a) {
                return 0;
            }
            const int now = conker::frame_input::mouse_speed();
            int next = now;
            if (horizontal < 0) {
                for (int speed : kMouseSpeeds) next = speed < now ? speed : next;
            } else {
                for (auto it = std::rbegin(kMouseSpeeds); it != std::rend(kMouseSpeeds); ++it) next = *it > now ? *it : next;
            }
            conker::frame_input::set_mouse_speed(next);
            return next != now && fresh_horizontal ? kMoveSound : 0;
        }
        default:
            break;
    }
    const int button = g_controls_row - kFirstMappedRow;
    if (pressed & input::kZ) {
        if (keys) {
            input::set_key_mapping(button, {});
        } else {
            input::set_mapping(port, button, 0);
        }
        return kSelectSound;
    }
    if (pressed & input::kL) {
        if (keys) {
            input::set_key_mapping(button, input::default_key_mapping(button));
        } else {
            input::set_mapping(port, button, input::default_mapping(button));
        }
        return kSelectSound;
    }
    if (!a && !(pressed & input::kR)) {
        return 0;
    }
    if (!keys && (port != 0 ? !input::connected(port) : input::controller_kind(0) == input::PadKind::None)) {
        show_message("NO PAD ON CONTROLLER " + std::to_string(port + 1));
        return kSelectSound;
    }
    g_listening = button;
    g_listen_adds = !a;
    g_listen_until = Clock::now() + kListenFor;
    if (keys) {
        input::start_listening_keys();
    } else {
        input::start_listening(port);
    }
    return kSelectSound;
}

int32_t mods_page(int vertical, int horizontal, bool fresh_horizontal, uint16_t pressed) {
    if (pressed & conker::input::kB) {
        g_page = Page::Main;
        return kSelectSound;
    }
    const int count = static_cast<int>(mods().size()) + (mods_changed() ? 1 : 0);
    if (count == 0) {
        return 0;
    }
    // A list like the reports': it stops at either end and scrolls to keep the chosen row in view.
    const auto keep_in_view = [](int rows) {
        g_mod_row = std::clamp(g_mod_row, 0, rows - 1);
        g_mod_top = std::clamp(g_mod_top, g_mod_row - kListRows + 1, g_mod_row);
        g_mod_top = std::clamp(g_mod_top, 0, std::max(rows - kListRows, 0));
    };
    if (vertical != 0) {
        g_mod_row += vertical;
        keep_in_view(count);
        return kMoveSound;
    }
    if (g_mod_row == static_cast<int>(mods().size())) {
        if (!(pressed & conker::input::kA)) {
            return 0;
        }
        // The same command line, as the online lobby starts a session (does not return if it can).
        std::printf("[conker] debug menu: starting again for the mods\n");
        conker::net::relaunch({});
        show_message("COULD NOT START AGAIN");
        return kSelectSound;
    }
    if (!(pressed & conker::input::kA) && !(horizontal != 0 && fresh_horizontal)) {
        return 0;
    }
    const Mod& mod = mods()[static_cast<size_t>(g_mod_row)];
    recomp::mods::enable_mod(mod.id, !mod_on(mod.id));  // librecomp writes mods.json
    std::printf("[conker] debug menu: mod %s %s\n", mod.id.c_str(), mod_on(mod.id) ? "on" : "off");
    keep_in_view(static_cast<int>(mods().size()) + (mods_changed() ? 1 : 0));  // the restart row comes and goes
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
    const bool at_checkpoint = g_page == Page::Reports && (pressed & conker::input::kR);
    if (!((pressed & conker::input::kA) || at_checkpoint) || count == 0) {
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
        if (conker::resume::load_report(rdram, g_snapshots[g_entry].path, &problem, at_checkpoint)) {
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
    conker::input::read_menu(&buttons, &x, &y);
    const uint16_t pressed = buttons & ~g_previous_buttons;
    g_previous_buttons = buttons;
    bool fresh_vertical, fresh_horizontal;
    const int up_down = ((buttons & conker::input::kDUp) || y > 0.5f) ? -1 : ((buttons & conker::input::kDDown) || y < -0.5f) ? 1 : 0;
    const int left_right = ((buttons & conker::input::kDLeft) || x < -0.5f) ? -1 : ((buttons & conker::input::kDRight) || x > 0.5f) ? 1 : 0;
    const int vertical = g_vertical.step(up_down, &fresh_vertical);
    const int horizontal = g_horizontal.step(left_right, &fresh_horizontal);

    // While a press on the pad is awaited (CONTROLS) the pad's buttons are that press: only Back closes.
    if (g_close_requested || (g_listening < 0 && ((pressed & conker::input::kStart) ||
                                                  (g_page == Page::Main && (pressed & conker::input::kB))))) {
        close_menu(rdram);
        return kSelectSound;
    }

    int32_t sound = 0;
    switch (g_page) {
        case Page::Main:
            sound = main_page(rdram, vertical, horizontal, fresh_horizontal, pressed);
            break;
        case Page::DebugView:
            sound = debug_view_page(vertical, horizontal, fresh_horizontal, pressed);
            break;
        case Page::Enhancements:
            sound = enhancements_page(vertical, horizontal, fresh_horizontal, pressed);
            break;
        case Page::Controls:
            sound = controls_page(vertical, horizontal, fresh_horizontal, pressed);
            break;
        case Page::Mods:
            sound = mods_page(vertical, horizontal, fresh_horizontal, pressed);
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
        y += 2 * kLineHeight;
    }
    if (conker::inspector::active()) {
        conker::inspector::report_pick(rdram);
        for (int line = 0; line < 4 && !g_active; line++) {
            const std::string text = conker::inspector::overlay_line(rdram, line);
            if (!text.empty()) {
                draw_text(rdram, ctx, kOverlayX, y, kLeft, line == 3 ? 0xFF80FFFF : 0x80FFFFFF, text);
                y += kLineHeight;
            }
        }
    }

    if (g_active && !g_controls_only && MEM_BU(0, kseg0(kPauseOpen)) == 0) {
        g_active = false;  // the pause went away by itself (the game quit to the title, say)
        g_holding = false;
        g_pads_held = true;
    }
    // On the MULTI door and the skull behind it, Start opens the online lobby (src/net/lobby_ui.hpp)
    // instead of going on: it is taken out of this frame's presses before the screen's handler reads
    // them (func_151E51EC copies D_800BE930 after this), and a line at the bottom says so. Not on
    // the modes or the join screen, where Start is the game's.
    static uint8_t last_door = 0xFF;  // the door selected before one was opened
    const bool title = MEM_BU(0, kseg0(kMenuHandler)) == kTitleScreen;
    const uint8_t door = MEM_BU(0, kseg0(kTitleDoor));
    if (title && door != kDoorOpened) {
        last_door = door;
    }
    const bool on_multi = title && (door == kMultiDoor ||
                                    (door == kDoorOpened && last_door == kMultiDoor && MEM_W(0, kseg0(kBarrelTurn)) == 0 &&
                                     MEM_BU(0, kseg0(kMultiJoin)) == 0));
    if (!g_active && on_multi && !conker::net::active() && !conker::lobby_ui::is_open()) {
        const char* start = conker::input::controller_kind() == conker::input::PadKind::PlayStation ? "OPTIONS"
                            : conker::input::controller_kind() == conker::input::PadKind::None ? "ENTER"
                                                                                               : "MENU";
        draw_text(rdram, ctx, kCenterX, kOnlinePromptY, kCentered, kChosenColor, std::string("PRESS ") + start + " TO PLAY ONLINE");
        const uint16_t pressed = MEM_HU(0, kseg0(kPadPressed));
        if (pressed & kStartButton) {
            MEM_H(0, kseg0(kPadPressed)) = static_cast<int16_t>(pressed & ~kStartButton);
            conker::lobby_ui::open();
        }
    }
    // The OPTIONS page's CONTROLS row: the controls page alone in the OPTIONS page's place, the
    // world going on behind it as it does behind OPTIONS (no pause).
    if (g_controls_requested) {
        g_controls_requested = false;
        if (!g_active && MEM_BU(0, kseg0(kMenuHandler)) != 0) {
            open_menu(rdram, true, false);
            g_page = Page::Controls;
            g_controls_only = true;
            g_controls_row = 0;
            g_controls_top = 0;
        }
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
                       {{"wireframe", g_wireframe}, {"show_fps", g_show_fps}, {"show_position", g_show_position},
                        {"inspect", conker::inspector::active()}});
}

bool conker::debug_menu::controls_available() { return !conker::net::active(); }

void conker::debug_menu::open_controls() {
    if (controls_available()) {
        g_controls_requested = true;
    }
}
