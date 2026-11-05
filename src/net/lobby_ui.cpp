// The ONLINE lobby's page; see lobby_ui.hpp.
#include "net/lobby_ui.hpp"

#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "enhancements/debug_menu/debug_menu.hpp"
#include "net/net_codes.hpp"
#include "net/net_lobby.hpp"
#include "net/net_play.hpp"
#include "platform/input.hpp"
#include "platform/process.hpp"

namespace conker::lobby_ui {

namespace {

namespace lobby = conker::net::lobby;
using Mode = lobby::Mode;
using PortState = conker::net::portmap::Status::State;
using conker::net::InPlace;

inline gpr kseg0(uint32_t address) { return static_cast<gpr>(static_cast<int32_t>(address)); }

// The title (screen 3, func_151DE8F0), found by watching memory while scripted presses turned the
// doors and went into MULTI. u8 D_800E0B1C is the door (5 GAME1, 0 MULTI, 1 OPTIONS, 2 CHAPTERS) and,
// behind MULTI's door, the barrel (4 the skull, then 6 WAR, 2 TANK, 7 RACE, 0 BACK, 5 BEACH, 3 RAPTOR,
// 1 HEIST); A on BACK leaves the room, so the page is not opened there. u8 D_800E0B2E is 1 in the barrel room
// (from going in until back out), u8 D_800E0B1A is nonzero while the camera moves in or out, f32
// D_800E0AF8 goes from 0 to 1 as a barrel turns its face up for the modes, and u8 D_800E0B30 is 4
// on the join screen and 0 before it.
constexpr uint32_t kMenuHandler = 0x800E0B94;
constexpr uint32_t kBarrel = 0x800E0B1C;
constexpr uint8_t kBackBarrel = 0;
constexpr uint32_t kBarrelRoom = 0x800E0B2E;
constexpr uint32_t kCameraMoving = 0x800E0B1A;
constexpr uint32_t kBarrelTurn = 0x800E0AF8;
constexpr uint32_t kMultiJoin = 0x800E0B30;
constexpr uint32_t kPadPressed = 0x800BE930;  // D_800BE930: controller 1's presses this frame
constexpr int kTitleScreen = 3;
constexpr uint16_t kPadA = 0x8000;
constexpr uint16_t kPadB = 0x4000;
// The barrels' own pictures: the skull (1980 to 1983, sharp_menus.cpp), the others' names (WAR is
// 2082 to 2084) and what a face shows for the modes (the plates, their names, the joystick: 1984 to
// 2067), found by logging what func_1510D0EC loaded on each.
constexpr uint32_t kBarrelSprites = 1980;
constexpr uint32_t kBarrelSpritesEnd = 2100;
constexpr uint32_t kSkullEnd = 1984;
constexpr uint32_t kBarrelNamesStart = 2068;

// The page, in the 292x216 picture: on the barrel's face, the info lines low on it.
constexpr int kX = conker::debug_menu::kPictureCenterX;
constexpr int kTitleY = 34;
constexpr int kTopY = 52;
constexpr int kLineHeight = 9;
constexpr int kRowHeight = 11;
constexpr int kInfoY = 164;
constexpr int kFieldX = 62;   // the left end of a field being typed
constexpr float kSmall = 0.8f;
constexpr float kRowScale = 0.9f;
constexpr uint32_t kGold = 0xCEC461FF;
constexpr uint32_t kWhite = 0xFFFFFFFF;
constexpr uint32_t kGrey = 0x9A9A9AFF;
constexpr uint32_t kRed = 0xFF8060FF;
constexpr uint32_t kGreen = 0x90E070FF;
constexpr uint32_t kYellow = 0xF0D060FF;
constexpr uint32_t kPlayerColors[] = {0xFF6050FF, 0x60A0FFFF, 0x70E060FF, 0xF0E050FF};  // as the MULTI join screen
constexpr size_t kLanShown = 4;     // LAN games listed at once
constexpr size_t kTextMax = 63;     // a text record's characters
constexpr size_t kNameMax = 24;
constexpr size_t kTargetMax = 60;   // an IPv6 code is 39, a host name and port more

// --- shared with the main thread --------------------------------------------------------------------

enum class Key { Up, Down, Left, Right, Enter, Escape, Backspace, Text };
struct KeyPress {
    Key key;
    std::string text;
};

std::mutex g_mutex;
std::deque<KeyPress> g_keys;     // under g_mutex: keys pressed on the page, for the game thread
std::string g_name;              // under g_mutex: the NAME row's text
std::string g_target;            // under g_mutex: the JOIN row's text
std::string g_clipboard_out;     // under g_mutex: text for the main thread to copy
std::atomic<bool> g_open{false};
std::atomic<bool> g_typing{false};  // a text field is being edited (the main thread starts SDL's text input)
std::atomic<uint64_t> g_opened_at{0};

SDL_Window* g_window = nullptr;

// CONKER_LOBBY_SCRIPT.
struct Step {
    int64_t at = 0;
    std::string action;
    std::string text;
};
std::vector<Step> g_script;
size_t g_script_next = 0;

uint64_t ticks() { return SDL_GetTicks64(); }

void push_key(Key key, std::string text = {}) {
    std::lock_guard lock(g_mutex);
    g_keys.push_back({key, std::move(text)});
}

std::string upper(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return text;
}

// --- the script -------------------------------------------------------------------------------------

void load_script() {
    const char* path = std::getenv("CONKER_LOBBY_SCRIPT");
    if (path == nullptr) {
        return;
    }
    std::ifstream file(path);
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::istringstream words(line);
        Step step;
        words >> step.at >> step.action;
        std::getline(words, step.text);
        step.text.erase(0, step.text.find_first_not_of(' '));
        g_script.push_back(step);
    }
    std::printf("[conker] lobby: script %s, %zu steps\n", path, g_script.size());
}

std::string mode_name(Mode mode) {
    switch (mode) {
        case Mode::Idle: return "idle";
        case Mode::Hosting: return "hosting";
        case Mode::Probing: return "probing";
        case Mode::Joined: return "joined";
        case Mode::Starting: return "starting";
    }
    return "?";
}

// The View as text, for a script's "shot" and the log.
std::string describe(const lobby::View& v) {
    std::string out = "mode " + mode_name(v.mode) + "\n";
    out += conker::net::internet_allowed() ? "internet UNSAFE on\n" : "internet off\n";
    out += conker::net::mods_on() ? "mods on " + conker::net::build_text() + "\n" : "mods none\n";
    for (const lobby::Player& p : v.players) {
        out += "player " + p.name + " | " + p.pad + " | ping " + std::to_string(p.ping) + (p.ready ? " | ready" : "") +
               (p.host ? " | host" : "") + (p.you ? " | you" : "") + "\n";
    }
    for (const lobby::Game& g : v.games) {
        out += "game " + g.name + " | " + g.address + " | " + std::to_string(g.players) + " players" +
               (g.playing ? " | playing" : " | lobby") + (g.problem.empty() ? "" : " | " + g.problem) + "\n";
    }
    for (const auto& [what, code] : v.codes) {
        out += "code " + what + " | " + code + "\n";
    }
    if (v.mode == Mode::Hosting) {
        out += "port " + v.port.message + "\n";
    }
    if (!v.key_check.empty()) {
        out += "key check " + v.key_check + "\n";
    }
    if (!v.settings_differ.empty()) {
        out += "differ " + v.settings_differ;
    }
    if (!v.error.empty()) {
        out += "error " + v.error + "\n";
    }
    if (!v.notice.empty()) {
        out += "notice " + v.notice + "\n";
    }
    return out;
}

void set_name_text(const std::string& name) {
    lobby::set_name(name);
    std::lock_guard lock(g_mutex);
    g_name = lobby::name();
}

void run_script() {
    while (g_script_next < g_script.size() &&
           static_cast<int64_t>(ticks() - g_opened_at.load()) >= g_script[g_script_next].at) {
        const Step& step = g_script[g_script_next];
        std::string lan_address;
        if (step.action == "joinlan") {
            // The LAN list's row for the game of that name; waits until the list has it.
            for (const lobby::Game& g : lobby::view().games) {
                if ((step.text.empty() || g.name == step.text) && g.problem.empty()) {
                    lan_address = g.address;
                    break;
                }
            }
            if (lan_address.empty()) {
                return;
            }
        }
        g_script_next++;
        std::printf("[conker] lobby: script: %s %s\n", step.action.c_str(), step.text.c_str());
        if (step.action == "name") {
            set_name_text(step.text);
        } else if (step.action == "host") {
            lobby::host(step.text.empty() ? conker::net::kDefaultPort : static_cast<uint16_t>(std::atoi(step.text.c_str())));
        } else if (step.action == "join") {
            lobby::join(step.text);
        } else if (step.action == "joinlan") {
            std::printf("[conker] lobby: script: LAN list has it at %s\n", lan_address.c_str());
            {
                std::lock_guard lock(g_mutex);
                g_target = lan_address;
            }
            lobby::join(lan_address);
        } else if (step.action == "ready") {
            lobby::set_ready(true);
        } else if (step.action == "start") {
            lobby::start();
        } else if (step.action == "kick") {
            lobby::kick(static_cast<size_t>(std::atoi(step.text.c_str())));
        } else if (step.action == "report") {
            conker::debug_menu::request_report();
        } else if (step.action == "leavesession") {
            conker::net::request_leave();
        } else if (step.action == "leave") {
            lobby::leave();
            lobby::browse();
        } else if (step.action == "quit") {
            // As the window's close button: the game quits the normal way (for tests of quitting).
            SDL_Event event{};
            event.type = SDL_QUIT;
            SDL_PushEvent(&event);
        } else if (step.action == "close") {
            close();
        } else if (step.action == "open") {
            open();
        } else if (step.action == "nomods") {
            // The ONLINE page's RESTART WITHOUT MODS, which shows only with mods on (so the restarted run,
            // which plays the script again, goes on).
            if (conker::net::mods_on()) {
                conker::net::restart_without_mods();
            }
        } else if (step.action == "setenv") {
            // For the session this machine starts again into (a test's pad script, say).
            const size_t equals = step.text.find('=');
            if (equals != std::string::npos) {
                process::set_env(step.text.substr(0, equals).c_str(), step.text.substr(equals + 1).c_str());
            }
        } else if (step.action == "shot") {
            const std::string text = describe(lobby::view());
            std::ofstream(step.text) << text;
            std::printf("[conker] lobby: view:\n%s", text.c_str());
        } else if (step.action == "key") {
            const std::string& k = step.text;
            if (k.rfind("text:", 0) == 0) push_key(Key::Text, k.substr(5));
            else if (k == "up") push_key(Key::Up);
            else if (k == "down") push_key(Key::Down);
            else if (k == "left") push_key(Key::Left);
            else if (k == "right") push_key(Key::Right);
            else if (k == "enter") push_key(Key::Enter);
            else if (k == "esc") push_key(Key::Escape);
            else if (k == "back") push_key(Key::Backspace);
        }
    }
}

// --- the page (game thread) -------------------------------------------------------------------------

enum class Action { None, Name, Host, Join, JoinGame, Close, Start, CopyCode, Kick, Leave, Cancel, BackToGame, LeaveSession,
                    RestartWithoutMods };

struct Row {
    std::string text;
    Action action = Action::None;
    bool enabled = true;
    size_t index = 0;              // the game (JoinGame) or the player (Kick)
    std::string info;              // the lines under the page while it is chosen
    uint32_t info_color = kGrey;
};

struct Line {
    std::string text;
    uint32_t color = kGrey;
};

struct Page {
    std::string title;
    std::vector<Line> lines;  // above the rows
    std::vector<Row> rows;
    std::vector<Line> info;   // under them, whatever row is chosen
};

// Holding a direction steps once, then again and again after a moment (frames at 30 a second).
struct Repeat {
    int held = 0;
    int frames = 0;
    int step(int direction) {
        if (direction != held) {
            held = direction;
            frames = 0;
            return direction;
        }
        if (direction == 0) {
            return 0;
        }
        frames++;
        return frames >= 10 && (frames - 10) % 3 == 0 ? direction : 0;
    }
};

enum class Field { None, Name, Target };

struct State {
    bool was_open = false;
    bool raised = false;        // A was pressed for the barrel's face to come up
    bool lowering = false;      // B was pressed for it to go back to the skull
    int chosen = 0;
    Mode last_mode = Mode::Idle;
    bool last_session = false;
    bool last_can_start = false;
    uint16_t previous = 0;
    Repeat vertical;
    Repeat horizontal;
    Field field = Field::None;
    std::string edit;           // the text being edited
    int key_row = 0;            // the barrel's keyboard
    int key_col = 0;
    int blink = 0;
} g;  // game thread

// The barrel's keyboard, for the pad: four rows of characters (the game's font has no _, and its ] and @
// come out as other signs), then
// SPACE, DELETE and DONE.
const char* const kKeyRows[] = {"ABCDEFGHIJK", "LMNOPQRSTUV", "WXYZ0123456", "789.:-/!?&+"};
constexpr int kKeyRowCount = 4;
constexpr int kKeyCols = 11;
const char* const kKeySpecials[] = {"SPACE", "DELETE", "DONE"};
constexpr int kKeySpecialCount = 3;

struct PadNames {
    const char* a;
    const char* b;
    const char* start;
    const char* z;
};

PadNames pad_names() {
    switch (conker::input::controller_kind()) {
        case conker::input::PadKind::PlayStation: return {"CROSS", "CIRCLE", "OPTIONS", "R2"};
        case conker::input::PadKind::None: return {"ENTER", "ESC", "ENTER", "BACKSPACE"};
        default: return {"A", "B", "MENU", "RT"};
    }
}

bool on_title(uint8_t* rdram) { return MEM_BU(0, kseg0(kMenuHandler)) == kTitleScreen; }

bool on_a_barrel(uint8_t* rdram) {
    return on_title(rdram) && MEM_BU(0, kseg0(kBarrelRoom)) == 1 && MEM_BU(0, kseg0(kCameraMoving)) == 0 &&
           MEM_BU(0, kseg0(kBarrel)) != kBackBarrel &&
           MEM_W(0, kseg0(kBarrelTurn)) == 0 && MEM_BU(0, kseg0(kMultiJoin)) == 0;
}

float barrel_turn(uint8_t* rdram) {
    const uint32_t bits = static_cast<uint32_t>(MEM_W(0, kseg0(kBarrelTurn)));
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// The game's memory is everyone's once a session is on (and from the frame it is entered): the page
// writes nothing there then.
bool session_on() { return conker::net::in_place_state(nullptr) == InPlace::kPlaying; }

void press_for_game(uint8_t* rdram, uint16_t button) {
    MEM_H(0, kseg0(kPadPressed)) = static_cast<int16_t>(MEM_HU(0, kseg0(kPadPressed)) | button);
}

// `text` broken at spaces into lines of at most `letters`, at most `max_lines` of them.
std::vector<std::string> wrap(const std::string& text, size_t letters, size_t max_lines) {
    std::vector<std::string> lines;
    std::string line;
    std::istringstream words(text);
    std::string word;
    while (words >> word) {
        if (word.size() > letters) word = word.substr(0, letters);
        if (!line.empty() && line.size() + 1 + word.size() > letters) {
            lines.push_back(line);
            line.clear();
        }
        line += (line.empty() ? "" : " ") + word;
    }
    if (!line.empty()) lines.push_back(line);
    if (lines.size() > max_lines) {
        lines.resize(max_lines);
        lines.back() = lines.back().substr(0, letters - 3) + "...";
    }
    return lines;
}

void add_wrapped(std::vector<Line>& out, const std::string& text, uint32_t color, size_t max_lines = 2) {
    for (const std::string& line : wrap(upper(text), 44, max_lines)) {
        out.push_back({line, color});
    }
}

std::string player_line(const lobby::Player& p, size_t i) {
    const std::string state = p.host ? "  HOST" : "";
    std::string ping;
    if (!(p.host && p.you)) {
        ping = p.ping < 0 ? "  ? MS" : "  " + std::to_string(p.ping) + " MS";
    }
    return "P" + std::to_string(i + 1) + "  " + upper(p.name) + (p.you ? " (YOU)" : "") + state + ping;
}

void add_players(Page& page, const lobby::View& v) {
    for (size_t i = 0; i < v.players.size(); i++) {
        page.lines.push_back({player_line(v.players[i], i), kPlayerColors[i % 4]});
    }
}

void add_messages(std::vector<Line>& out, const lobby::View& v, const std::string& session_message) {
    if (!session_message.empty()) {
        add_wrapped(out, session_message, kRed);
    }
    if (!v.error.empty()) {
        add_wrapped(out, v.error, kRed);
    } else if (!v.notice.empty() && v.mode != Mode::Joined) {
        add_wrapped(out, v.notice, kGreen);
    }
}

std::string field_text(const std::string& text, const std::string& empty) {
    if (text.empty()) {
        return empty;
    }
    return upper(text.size() > 26 ? "..." + text.substr(text.size() - 23) : text);
}

Page build_page(const lobby::View& v) {
    Page page;
    std::string session_message;
    const InPlace session = conker::net::in_place_state(&session_message);
    if (session == InPlace::kStarting) {
        page.title = "ONLINE";
        page.lines.push_back({"STARTING THE SESSION...", kGreen});
        page.rows.push_back({"CANCEL", Action::Cancel});
        return page;
    }
    std::string name, target;
    {
        std::lock_guard lock(g_mutex);
        name = g_name;
        target = g_target;
    }
    const bool internet = conker::net::internet_allowed();
    switch (v.mode) {
        case Mode::Idle: {
            page.title = "ONLINE";
            page.rows.push_back({"NAME  " + field_text(name, "PLAYER"), Action::Name, true, 0,
                                 "THE NAME THE OTHER PLAYERS SEE"});
            page.rows.push_back({"HOST A GAME", Action::Host, true, 0,
                                 "ON UDP PORT " + std::to_string(conker::net::kDefaultPort) + "  THE OTHERS JOIN YOU"});
            page.rows.push_back({"JOIN  " + field_text(target, "ADDRESS OR CODE"), Action::Join, true, 0,
                                 "THE HOST'S ADDRESS OR JOIN CODE"});
            if (conker::net::mods_on()) {
                // Mods loaded at boot cannot be unloaded, and a lobby game needs everyone on the same
                // ones: a run without any plays with everyone else who has none.
                page.lines.push_back({"MODS ARE ON  ONLINE GAMES NEED NONE", kYellow});
                page.rows.push_back({"RESTART WITHOUT MODS", Action::RestartWithoutMods, true, 0,
                                     "THIS RUN ONLY  YOUR MODS STAY ON FOR NEXT TIME", kYellow});
            }
            if (v.games.empty()) {
                page.lines.push_back({"", kGrey});
            }
            for (size_t i = 0; i < std::min(v.games.size(), kLanShown); i++) {
                const lobby::Game& game = v.games[i];
                Row row{"JOIN " + upper(game.name) + "  " + std::to_string(game.players) + "P" + (game.playing ? "  PLAYING" : ""),
                        Action::JoinGame, game.problem.empty(), i, "ON THIS NETWORK AT " + game.address};
                if (!game.problem.empty()) {
                    row.info = upper(game.problem);
                    row.info_color = kYellow;
                }
                page.rows.push_back(row);
            }
            page.rows.push_back({"CLOSE", Action::Close, true, 0, "BACK TO THE BARREL"});
            if (v.games.empty()) {
                page.info.push_back({"LOOKING FOR GAMES ON THIS NETWORK...", kGrey});
            }
            page.info.push_back({"JOIN ONLY HOSTS YOU TRUST", kYellow});
            break;
        }
        case Mode::Hosting: {
            page.title = "HOSTING";
            add_players(page, v);
            page.rows.push_back({"START", Action::Start, v.can_start, 0,
                                 v.can_start ? "EVERYONE HERE PLAYS" : "WAITING FOR PLAYERS..."});
            if (!v.codes.empty()) {
                page.rows.push_back({"COPY JOIN CODE", Action::CopyCode, true, 0, "TO THE CLIPBOARD, TO GIVE YOUR FRIENDS"});
            }
            for (size_t i = 1; i < v.players.size(); i++) {
                page.rows.push_back({"REMOVE P" + std::to_string(i + 1), Action::Kick, true, i,
                                     "THEIR MACHINE CANNOT JOIN AGAIN"});
            }
            page.rows.push_back({"LEAVE", Action::Leave, true, 0, "CLOSES THE LOBBY"});
            for (size_t i = 0; i < std::min<size_t>(v.codes.size(), 2); i++) {
                page.info.push_back({"CODE  " + v.codes[i].second, kWhite});
            }
            page.info.push_back({"KEY CHECK  " + upper(v.key_check), kGrey});
            const uint32_t port_color = v.port.state == PortState::Opened ? kGreen : v.port.state == PortState::Failed ? kYellow : kGrey;
            if (!v.port.message.empty() && internet) {
                add_wrapped(page.info, v.port.message, port_color, 1);
            }
            break;
        }
        case Mode::Joined: {
            page.title = "IN A LOBBY";
            add_players(page, v);
            page.lines.push_back({"", kGrey});
            page.lines.push_back({"WAITING FOR THE HOST TO START", kGreen});
            page.rows.push_back({"LEAVE", Action::Leave, true, 0, "BACK TO THE LIST"});
            page.info.push_back({"HOST KEY CHECK  " + upper(v.key_check), kGrey});
            if (!v.settings_differ.empty()) {
                std::string differ = v.settings_differ;
                std::replace(differ.begin(), differ.end(), '\n', ' ');
                add_wrapped(page.info, "THE HOST'S SETTINGS: " + differ, kYellow);
            }
            break;
        }
        case Mode::Probing:
            page.title = "ONLINE";
            add_wrapped(page.lines, v.notice.empty() ? "ASKING THE HOST..." : v.notice, kGreen);
            page.rows.push_back({"CANCEL", Action::Leave});
            break;
        case Mode::Starting:
            page.title = "ONLINE";
            add_wrapped(page.lines, v.notice.empty() ? "STARTING THE SESSION..." : v.notice, kGreen);
            break;
    }
    add_messages(page.info, v, session_message);
    page.info.push_back(internet ? Line{"UNSAFE  INTERNET PLAY IS ON", kRed} : Line{"LAN ONLY  INTERNET PLAY IS OFF", kGrey});
    return page;
}

void start_editing(Field field) {
    std::lock_guard lock(g_mutex);
    g.field = field;
    g.edit = field == Field::Name ? g_name : g_target;
    g.key_row = 0;
    g.key_col = 0;
    g_typing.store(true);
}

void finish_editing(bool keep) {
    const Field field = g.field;
    g.field = Field::None;
    g_typing.store(false);
    if (!keep) {
        return;
    }
    if (field == Field::Name) {
        set_name_text(g.edit);
    } else {
        std::string target = g.edit;
        target.erase(0, target.find_first_not_of(' '));
        target.erase(target.find_last_not_of(' ') + 1);
        {
            std::lock_guard lock(g_mutex);
            g_target = target;
        }
        if (!target.empty()) {
            lobby::join(target);
        }
    }
}

void type_text(const std::string& text) {
    const size_t max = g.field == Field::Name ? kNameMax : kTargetMax;
    for (const char c : text) {
        if (c >= 0x20 && c < 0x7F && g.edit.size() < max) {
            g.edit += c;
        }
    }
}

// What a row does; true if it made a sound worth a select.
void activate(const Row& row, const lobby::View& v) {
    switch (row.action) {
        case Action::Name: start_editing(Field::Name); break;
        case Action::Join: start_editing(Field::Target); break;
        case Action::Host: lobby::host(conker::net::kDefaultPort); break;
        case Action::JoinGame:
            if (row.index < v.games.size()) {
                {
                    std::lock_guard lock(g_mutex);
                    g_target = v.games[row.index].address;
                }
                lobby::join(v.games[row.index].address);
            }
            break;
        case Action::Close: close(); break;
        case Action::Start: lobby::start(); break;
        case Action::CopyCode:
            if (!v.codes.empty()) {
                std::lock_guard lock(g_mutex);
                g_clipboard_out = v.codes[0].second;
            }
            break;
        case Action::Kick: lobby::kick(row.index); break;
        case Action::Leave:
            lobby::leave();
            lobby::browse();
            break;
        case Action::Cancel: conker::net::cancel_in_place(); break;
        case Action::RestartWithoutMods: conker::net::restart_without_mods(); break;
        case Action::BackToGame: close(); break;
        case Action::LeaveSession:
            conker::net::request_leave();
            close();
            break;
        case Action::None: break;
    }
}

// Back from the page: leave a lobby, or close from the first page.
void go_back(const lobby::View& v) {
    if (conker::net::in_place_state(nullptr) == InPlace::kStarting) {
        return;
    }
    if (v.mode == Mode::Idle) {
        close();
    } else if (v.mode != Mode::Starting) {
        lobby::leave();
        lobby::browse();
    }
}

struct Input {
    int vertical = 0;
    int horizontal = 0;
    bool a = false;
    bool b = false;
    bool start = false;
    bool z = false;
    std::vector<KeyPress> keys;
};

Input read_input() {
    Input in;
    uint16_t buttons;
    float x, y;
    conker::input::read_menu_pad(&buttons, &x, &y);
    const uint16_t pressed = buttons & ~g.previous;
    g.previous = buttons;
    namespace input = conker::input;
    const int up_down = ((buttons & input::kDUp) || y > 0.5f) ? -1 : ((buttons & input::kDDown) || y < -0.5f) ? 1 : 0;
    const int left_right = ((buttons & input::kDLeft) || x < -0.5f) ? -1 : ((buttons & input::kDRight) || x > 0.5f) ? 1 : 0;
    in.vertical = g.vertical.step(up_down);
    in.horizontal = g.horizontal.step(left_right);
    in.a = (pressed & input::kA) != 0;
    in.b = (pressed & input::kB) != 0;
    in.start = (pressed & input::kStart) != 0;
    in.z = (pressed & input::kZ) != 0;
    std::lock_guard lock(g_mutex);
    in.keys.assign(g_keys.begin(), g_keys.end());
    g_keys.clear();
    return in;
}

// --- the barrel's keyboard --------------------------------------------------------------------------

int edit_frame(uint8_t* rdram, recomp_context* ctx, const Input& in) {
    int sound = 0;  // 1 move, 2 select
    for (const KeyPress& k : in.keys) {
        switch (k.key) {
            case Key::Text: type_text(k.text); break;
            case Key::Backspace:
                if (!g.edit.empty()) g.edit.pop_back();
                break;
            case Key::Enter: finish_editing(true); return 2;
            case Key::Escape: finish_editing(false); return 2;
            case Key::Up: g.key_row = std::max(0, g.key_row - 1); sound = 1; break;
            case Key::Down: g.key_row = std::min(kKeyRowCount, g.key_row + 1); sound = 1; break;
            case Key::Left: g.key_col--; sound = 1; break;
            case Key::Right: g.key_col++; sound = 1; break;
        }
    }
    if (in.vertical != 0) {
        g.key_row = (g.key_row + in.vertical + kKeyRowCount + 1) % (kKeyRowCount + 1);
        sound = 1;
    }
    if (in.horizontal != 0) {
        g.key_col += in.horizontal;
        sound = 1;
    }
    const int cols = g.key_row == kKeyRowCount ? kKeySpecialCount : kKeyCols;
    if (g.key_row == kKeyRowCount && (in.vertical != 0) && g.key_col >= kKeySpecialCount) {
        g.key_col = g.key_col * kKeySpecialCount / kKeyCols;  // from a letter column to the special under it
    }
    g.key_col = (g.key_col % cols + cols) % cols;
    if (in.start) {
        finish_editing(true);
        return 2;
    }
    // B goes back (what was typed is not kept), as everywhere on the page; Z deletes a letter.
    if (in.b) {
        finish_editing(false);
        return 2;
    }
    if (in.z && !g.edit.empty()) {
        g.edit.pop_back();
        sound = 1;
    }
    if (in.a) {
        if (g.key_row < kKeyRowCount) {
            type_text(std::string(1, kKeyRows[g.key_row][g.key_col]));
        } else if (g.key_col == 0) {
            type_text(" ");
        } else if (g.key_col == 1) {
            if (!g.edit.empty()) g.edit.pop_back();
        } else {
            finish_editing(true);
        }
        sound = 2;
    }
    // The picture.
    namespace dm = conker::debug_menu;
    dm::draw_text(rdram, ctx, kX, kTitleY, true, kGold, g.field == Field::Name ? "YOUR NAME" : "JOIN", 1.0f);
    // Left-aligned, so the blinking cursor (a dash: the font has no _) does not move the text.
    std::string shown = upper(g.edit);
    if (shown.size() > 26) shown = "..." + shown.substr(shown.size() - 23);
    g.blink = (g.blink + 1) % 30;
    dm::draw_text(rdram, ctx, kFieldX, kTopY + 4, false, kWhite, shown + (g.blink < 15 ? "-" : ""), 1.0f);
    if (g.field == Field::Target && g.edit.empty()) {
        dm::draw_text(rdram, ctx, kX, kTopY + 16, true, kGrey, "AN ADDRESS (192.168.1.20) OR A JOIN CODE", kSmall);
    }
    constexpr int kKeysY = 84;
    constexpr int kKeyWidth = 12;
    constexpr int kKeyHeight = 12;
    for (int r = 0; r < kKeyRowCount; r++) {
        for (int c = 0; c < kKeyCols; c++) {
            const bool chosen = r == g.key_row && c == g.key_col;
            dm::draw_text(rdram, ctx, kX + (2 * c - (kKeyCols - 1)) * kKeyWidth / 2, kKeysY + r * kKeyHeight, true,
                          chosen ? kWhite : kGrey, std::string(1, kKeyRows[r][c]), chosen ? 1.1f : kRowScale);
        }
    }
    for (int c = 0; c < kKeySpecialCount; c++) {
        const bool chosen = g.key_row == kKeyRowCount && c == g.key_col;
        dm::draw_text(rdram, ctx, kX + (c - 1) * 44, kKeysY + kKeyRowCount * kKeyHeight + 2, true, chosen ? kWhite : kGrey,
                      kKeySpecials[c], chosen ? 1.0f : kRowScale);
    }
    const PadNames names = pad_names();
    // Short lines: the page is the 4:3 picture's middle.
    int hint_y = kInfoY + 4;
    if (conker::input::controller_kind() != conker::input::PadKind::None) {
        dm::draw_text(rdram, ctx, kX, hint_y, true, kGrey,
                      std::string(names.a) + " ADDS  " + names.z + " DELETES  " + names.start + " IS DONE", kSmall);
        hint_y += kLineHeight;
        dm::draw_text(rdram, ctx, kX, hint_y, true, kGrey, std::string(names.b) + " GOES BACK", kSmall);
        hint_y += kLineHeight;
    }
    dm::draw_text(rdram, ctx, kX, hint_y, true, kGrey, "OR TYPE  ENTER IS DONE  ESC GOES BACK", kSmall);
    return sound;
}

// --- the session's page (drawn by net_play, see the top) ---------------------------------------------

void session_frame(const Input& in) {
    static int chosen = 0;
    bool back = in.b;
    bool select = in.a || in.start;
    int vertical = in.vertical;
    for (const KeyPress& k : in.keys) {
        if (k.key == Key::Up) vertical = -1;
        if (k.key == Key::Down) vertical = 1;
        if (k.key == Key::Enter) select = true;
        if (k.key == Key::Escape) back = true;
    }
    chosen = (chosen + vertical + 3) % 3;
    if (back) {
        close();
        return;
    }
    if (select && chosen == 2) {
        // Where this machine's player is, with the camera, to show someone a spot (the debug menu,
        // which has the same row, is off online). The page stays open to show where it went.
        conker::debug_menu::request_report();
        select = false;
    }
    if (select) {
        if (chosen == 1) {
            conker::net::request_leave();
        }
        chosen = 0;
        close();
        return;
    }
    conker::net::SessionMenu menu;
    menu.title = "ONLINE SESSION";
    menu.lines.push_back(upper(conker::net::status_line()));
    menu.lines.push_back("LEAVING BRINGS YOUR OWN GAME BACK");
    const std::string report = conker::debug_menu::report_result();
    if (!report.empty()) {
        menu.lines.push_back(upper(report));
    }
    menu.rows = {"BACK TO THE GAME", "LEAVE THE SESSION", "SAVE DEBUG REPORT"};
    menu.chosen = chosen;
    const PadNames names = pad_names();
    menu.hint = std::string(names.a) + " CHOOSES  " + names.b + " GOES BACK";
    conker::net::set_session_menu(menu);
}

}  // namespace

// --- the main thread ----------------------------------------------------------------------------------

void set_window(SDL_Window* window) {
    g_window = window;
    {
        std::lock_guard lock(g_mutex);
        g_name = lobby::name();
        g_target = lobby::last_join();
    }
    load_script();
    if (conker::net::lobby_at_boot()) {
        open();
    }
}

void open() {
    if (conker::net::active() && !conker::net::in_place()) {
        return;  // a session from the command line: nothing to do here
    }
    if (!g_open.exchange(true)) {
        uint64_t zero = 0;
        g_opened_at.compare_exchange_strong(zero, ticks());
        {
            std::lock_guard lock(g_mutex);
            g_keys.clear();
            g_name = lobby::name();
            if (g_target.empty()) {
                g_target = lobby::last_join();
            }
        }
        conker::input::set_blocked(true);
        if (!conker::net::in_place()) {
            lobby::browse();
        }
        std::printf("[conker] lobby: open\n");
    }
}

void close() {
    if (g_open.exchange(false)) {
        g_typing.store(false);
        if (lobby::view().mode != Mode::Starting && !conker::net::in_place()) {
            lobby::leave();
        }
        conker::net::set_session_menu({});
        conker::input::set_blocked(false);
        std::printf("[conker] lobby: closed\n");
    }
}

bool is_open() { return g_open.load(); }

bool handle_event(const SDL_Event& event) {
    if (!g_open.load()) {
        return false;
    }
    if (event.type == SDL_TEXTINPUT) {
        if (g_typing.load()) {
            push_key(Key::Text, event.text.text);
        }
        return true;
    }
    if (event.type != SDL_KEYDOWN) {
        return event.type == SDL_KEYUP;
    }
    const SDL_Keymod mods = SDL_GetModState();
    const bool shortcut = (mods & (KMOD_GUI | KMOD_CTRL)) != 0;
    // The keys the game's controls give the stick (W A S D) and A, B and Start steer the page too,
    // except while a field is being typed, where they are letters.
    if (!g_typing.load() && !shortcut) {
        const int scancode = event.key.keysym.scancode;
        const auto mapped = [&](int row) {
            const std::vector<int> keys = conker::input::key_mapping(row);
            return std::find(keys.begin(), keys.end(), scancode) != keys.end();
        };
        namespace input = conker::input;
        constexpr int kStick = input::kMappedButtonCount;  // STICK UP, DOWN, LEFT, RIGHT
        const Key stick_keys[] = {Key::Up, Key::Down, Key::Left, Key::Right};
        for (int i = 0; i < 4; i++) {
            if (mapped(kStick + i)) {
                push_key(stick_keys[i]);
                return true;
            }
        }
        for (int i = 0; i < input::kMappedButtonCount; i++) {
            const uint16_t n64 = input::kMappedButtons[i].n64;
            if ((n64 == input::kA || n64 == input::kStart || n64 == input::kB) && mapped(i)) {
                if (event.key.repeat == 0) {
                    push_key(n64 == input::kB ? Key::Escape : Key::Enter);
                }
                return true;
            }
        }
    }
    switch (event.key.keysym.scancode) {
        case SDL_SCANCODE_UP: push_key(Key::Up); break;
        case SDL_SCANCODE_DOWN: push_key(Key::Down); break;
        case SDL_SCANCODE_LEFT: push_key(Key::Left); break;
        case SDL_SCANCODE_RIGHT: push_key(Key::Right); break;
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER:
            if (event.key.repeat == 0 && !shortcut && (mods & KMOD_ALT) == 0) push_key(Key::Enter);
            break;
        case SDL_SCANCODE_SPACE:
            if (!g_typing.load() && event.key.repeat == 0) push_key(Key::Enter);
            break;
        case SDL_SCANCODE_ESCAPE:
            if (event.key.repeat == 0) push_key(Key::Escape);
            break;
        case SDL_SCANCODE_BACKSPACE: push_key(Key::Backspace); break;
        case SDL_SCANCODE_V:
            if (shortcut && g_typing.load()) {
                char* text = SDL_GetClipboardText();
                std::string pasted = text != nullptr ? text : "";
                SDL_free(text);
                std::replace(pasted.begin(), pasted.end(), '\n', ' ');
                push_key(Key::Text, pasted);
            }
            break;
        default: break;
    }
    return true;
}

void update() {
    if (!g_script.empty() && g_opened_at.load() != 0) {
        run_script();
    }
    // The session the lobby started is on: the page gives the game back.
    static InPlace was = InPlace::kNone;
    const InPlace session = conker::net::in_place_state(nullptr);
    if (was == InPlace::kStarting && session == InPlace::kPlaying) {
        close();
    }
    was = session;
    std::string copy;
    {
        std::lock_guard lock(g_mutex);
        copy.swap(g_clipboard_out);
    }
    if (!copy.empty()) {
        SDL_SetClipboardText(copy.c_str());
    }
    static bool text_input = false;
    const bool wanted = g_typing.load() && g_open.load();
    if (wanted != text_input) {
        text_input = wanted;
        if (wanted) {
            SDL_StartTextInput();
        } else {
            SDL_StopTextInput();
        }
    }
}

// --- the game thread -----------------------------------------------------------------------------------

void frame(uint8_t* rdram, recomp_context* ctx) {
    const bool open = g_open.load();
    const bool session = session_on();
    if (open && !g.was_open) {
        g.was_open = true;
        g.chosen = 0;
        g.field = Field::None;
        g_typing.store(false);
        g.lowering = false;
        uint16_t buttons;
        float x, y;
        conker::input::read_menu_pad(&buttons, &x, &y);
        g.previous = buttons;  // the press that opened the page is not the page's
        // On a barrel, A brings its face up, where the page goes.
        if (!session && on_a_barrel(rdram)) {
            press_for_game(rdram, kPadA);
            g.raised = true;
        }
    } else if (!open && g.was_open) {
        g.was_open = false;
        g.field = Field::None;
        g_typing.store(false);
        // B turns it back to the skull (not in a session: the game is everyone's then).
        if (g.raised && !session && !conker::net::in_place() && on_title(rdram) && barrel_turn(rdram) > 0.0f) {
            press_for_game(rdram, kPadB);
            g.lowering = true;
        }
        g.raised = false;
    }
    if (g.lowering && (!on_title(rdram) || barrel_turn(rdram) == 0.0f || session)) {
        g.lowering = false;
    }
    if (!open) {
        return;
    }
    const Input in = read_input();
    if (session) {
        session_frame(in);
        return;
    }
    conker::net::set_session_menu({});
    namespace dm = conker::debug_menu;
    int sound = 0;
    if (g.field != Field::None) {
        sound = edit_frame(rdram, ctx, in);
        if (sound != 0) {
            dm::play_menu_sound(rdram, ctx, sound == 2);
        }
        return;
    }
    const lobby::View v = lobby::view();
    const bool now_session = conker::net::in_place_state(nullptr) != InPlace::kNone;
    // A new page, or START can be pressed now: the first row (START, once everyone is ready).
    if (v.mode != g.last_mode || now_session != g.last_session || v.can_start != g.last_can_start) {
        g.last_mode = v.mode;
        g.last_session = now_session;
        g.last_can_start = v.can_start;
        g.chosen = 0;
    }
    const Page page = build_page(v);
    const int count = static_cast<int>(page.rows.size());
    int vertical = in.vertical;
    bool select = in.a || in.start;
    bool back = in.b;
    for (size_t i = 0; i < in.keys.size(); i++) {
        const KeyPress& k = in.keys[i];
        if (k.key == Key::Up) vertical = -1;
        if (k.key == Key::Down) vertical = 1;
        if (k.key == Key::Enter) select = true;
        if (k.key == Key::Escape || k.key == Key::Backspace) back = true;
        if (select || back) {
            // The keys after a press are for what it opens (JOIN's field, say): the next frame's.
            std::lock_guard lock(g_mutex);
            g_keys.insert(g_keys.begin(), in.keys.begin() + static_cast<std::ptrdiff_t>(i) + 1, in.keys.end());
            break;
        }
    }
    if (count > 0) {
        g.chosen = std::clamp(g.chosen, 0, count - 1);
        // A row that cannot be pressed (START before everyone is ready) is stepped over.
        const int step = vertical != 0 ? vertical : 1;
        int moved = vertical != 0 ? g.chosen + vertical : g.chosen;
        for (int tries = 0; tries < count; tries++) {
            moved = (moved + count) % count;
            if (page.rows[moved].enabled) break;
            moved += step;
        }
        if (moved != g.chosen) {
            g.chosen = moved;
            sound = vertical != 0 ? 1 : sound;
        }
    }
    // Steering may close the page (CLOSE, BACK): it is drawn this frame all the same.
    if (select && count > 0) {
        const Row& row = page.rows[g.chosen];
        if (row.enabled) {
            activate(row, v);
            sound = 2;
        }
    } else if (back) {
        go_back(v);
        sound = 2;
    }
    if (sound != 0) {
        dm::play_menu_sound(rdram, ctx, sound == 2);
    }
    if (g.field != Field::None) {
        return;  // NAME or JOIN: the keyboard from the next frame
    }

    dm::draw_text(rdram, ctx, kX, kTitleY, true, kGold, page.title, 1.0f);
    int y = kTopY;
    for (const Line& line : page.lines) {
        dm::draw_text(rdram, ctx, kX, y, true, line.color, line.text.substr(0, kTextMax), kSmall);
        y += kLineHeight;
    }
    if (!page.lines.empty()) {
        y += 4;
    }
    for (int i = 0; i < count; i++) {
        const Row& row = page.rows[i];
        const bool chosen = i == g.chosen;
        const uint32_t color = !row.enabled ? 0x606060FF : chosen ? kWhite : kGrey;
        dm::draw_text(rdram, ctx, kX, y, true, color, row.text.substr(0, kTextMax), chosen ? 1.0f : kRowScale);
        y += kRowHeight;
    }
    std::vector<Line> info;
    if (count > 0 && !page.rows[g.chosen].info.empty()) {
        info.push_back({page.rows[g.chosen].info, page.rows[g.chosen].info_color});
    }
    info.insert(info.end(), page.info.begin(), page.info.end());
    // The info lines from kInfoY down, pushed up when there are many.
    int info_y = std::min(kInfoY, 206 - static_cast<int>(info.size()) * kLineHeight);
    info_y = std::max(info_y, y + 2);
    for (const Line& line : info) {
        if (info_y > 206) break;
        dm::draw_text(rdram, ctx, kX, info_y, true, line.color, line.text.substr(0, kTextMax), kSmall);
        info_y += kLineHeight;
    }
}

bool on_barrels(uint8_t* rdram) { return on_a_barrel(rdram); }

bool hides_sprite(uint32_t resource) {
    if (resource < kBarrelSprites || resource >= kBarrelSpritesEnd) {
        return false;
    }
    if (g.lowering) {
        return resource >= kSkullEnd && resource < kBarrelNamesStart;  // the face's pictures, while it turns back
    }
    return g.raised && g_open.load() && !session_on();
}

}  // namespace conker::lobby_ui
