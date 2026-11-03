// The ONLINE lobby window; see lobby_ui.hpp.
#include "net/lobby_ui.hpp"

#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "gui/rt64_inspector.h"
#include "hle/rt64_application.h"
#include "imgui/imgui.h"
#include "imgui/imgui_impl_sdl2_custom.h"
#include "platform/input.hpp"
#include "net/lobby_style.hpp"
#include "platform/process.hpp"
#include "rhi/rt64_render_hooks.h"
#include "net/net_codes.hpp"
#include "net/net_lobby.hpp"
#include "net/net_play.hpp"

namespace conker::lobby_ui {

namespace {

namespace lobby = conker::net::lobby;
using Mode = lobby::Mode;
using PortState = conker::net::portmap::Status::State;

std::atomic<RT64::Application*> g_app{nullptr};
SDL_Window* g_window = nullptr;  // main thread
// The ImGui context and its Metal/Vulkan/D3D12 drawing: RT64's own inspector class, which brings
// both. Made on the main thread when the window first opens; drawn by the present thread.
std::unique_ptr<RT64::Inspector> g_imgui;
std::atomic<bool> g_open{false};
std::atomic<bool> g_frame_ready{false};  // a frame was built since the window opened
uint64_t g_last_frame = 0;
float g_scale = 0.0f;
std::string g_clipboard;                 // main thread: the last text read from the clipboard

// What the window shows (main thread).
enum class Page { Main, Lobby };
char g_name[32] = {};
char g_target[128] = {};
bool g_focus_first = true;  // put the pad's highlight on the page's first button
Mode g_last_mode = Mode::Idle;

// CONKER_LOBBY_SCRIPT.
struct Step {
    int64_t at = 0;
    std::string action;
    std::string text;
};
std::vector<Step> g_script;
size_t g_script_next = 0;
uint64_t g_opened_at = 0;

uint64_t ticks() { return SDL_GetTicks64(); }

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

void run_script() {
    while (g_script_next < g_script.size() &&
           static_cast<int64_t>(ticks() - g_opened_at) >= g_script[g_script_next].at) {
        const Step& step = g_script[g_script_next];
        std::string lan_address;
        if (step.action == "joinlan") {
            // The LAN list's JOIN button for the game of that name; waits until the list has it.
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
            lobby::set_name(step.text);
            std::snprintf(g_name, sizeof(g_name), "%s", lobby::name().c_str());
        } else if (step.action == "host") {
            lobby::host(step.text.empty() ? conker::net::kDefaultPort : static_cast<uint16_t>(std::atoi(step.text.c_str())));
        } else if (step.action == "join") {
            lobby::join(step.text);
        } else if (step.action == "joinlan") {
            std::printf("[conker] lobby: script: LAN list has it at %s\n", lan_address.c_str());
            std::snprintf(g_target, sizeof(g_target), "%s", lan_address.c_str());
            lobby::join(lan_address);
        } else if (step.action == "ready") {
            lobby::set_ready(true);
        } else if (step.action == "start") {
            lobby::start();
        } else if (step.action == "kick") {
            lobby::kick(static_cast<size_t>(std::atoi(step.text.c_str())));
        } else if (step.action == "leave") {
            lobby::leave();
            lobby::browse();
        } else if (step.action == "close") {
            close();
        } else if (step.action == "open") {
            open();
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
        }
    }
}

bool make_imgui() {
    if (g_imgui != nullptr) {
        return true;
    }
    RT64::Application* app = g_app.load();
    if (app == nullptr || g_window == nullptr || app->device == nullptr || app->swapChain == nullptr) {
        return false;
    }
    ImGuiContext* other = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(nullptr);
    // It also sets up ImGui's SDL side for the window, which feeds it events (handle_event) and starts
    // each frame (Inspector::newFrame), both here on the main thread.
    g_imgui = std::make_unique<RT64::Inspector>(app->device.get(), app->swapChain.get(), app->chosenGraphicsAPI,
                                                g_window);
    if (ImGui::GetCurrentContext() == nullptr) {
        g_imgui.reset();
        ImGui::SetCurrentContext(other);
        return false;
    }
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    // Changing the cursor and the text input rectangle are window calls that this thread may make,
    // but the clipboard is read here too: keep all of SDL on the main thread (this one).
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad |
                      ImGuiConfigFlags_NoMouseCursorChange;
    ImGuiPlatformIO& platform = ImGui::GetPlatformIO();
    platform.Platform_GetClipboardTextFn = [](ImGuiContext*) -> const char* {
        char* text = SDL_GetClipboardText();
        g_clipboard = text != nullptr ? text : "";
        SDL_free(text);
        return g_clipboard.c_str();
    };
    platform.Platform_SetClipboardTextFn = [](ImGuiContext*, const char* text) { SDL_SetClipboardText(text); };
    platform.Platform_SetImeDataFn = nullptr;
    ImGui_ImplSDL2_SetMouseCaptureMode(ImGui_ImplSDL2_MouseCaptureMode_Disabled);
    return true;
}

void text_wrapped(const std::string& text, ImU32 color = 0) {
    if (color != 0) {
        ImGui::PushStyleColor(ImGuiCol_Text, color);
    }
    ImGui::TextWrapped("%s", text.c_str());
    if (color != 0) {
        ImGui::PopStyleColor();
    }
}

namespace style = conker::lobby_style;

void players_table(const lobby::View& v) {
    if (!ImGui::BeginTable("players", 4, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        return;
    }
    ImGui::TableSetupColumn("PLAYER", ImGuiTableColumnFlags_WidthStretch, 3.0f);
    ImGui::TableSetupColumn("PAD", ImGuiTableColumnFlags_WidthStretch, 4.0f);
    ImGui::TableSetupColumn("PING", ImGuiTableColumnFlags_WidthStretch, 1.2f);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 1.6f);
    ImGui::TableHeadersRow();
    for (size_t i = 0; i < v.players.size(); i++) {
        const lobby::Player& p = v.players[i];
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextColored(ImColor(style::player(static_cast<int>(i))), "P%zu", i + 1);
        ImGui::SameLine();
        ImGui::Text("%s%s", p.name.c_str(), p.you ? " (you)" : "");
        ImGui::TableNextColumn();
        ImGui::TextColored(ImColor(style::grey()), "%s", p.pad.c_str());
        ImGui::TableNextColumn();
        if (p.host && p.you) {
            ImGui::TextColored(ImColor(style::grey()), "-");
        } else if (p.ping < 0) {
            ImGui::TextColored(ImColor(style::grey()), "?");
        } else {
            ImGui::TextColored(ImColor(p.ping > 150 ? style::yellow() : style::green()), "%d", p.ping);
        }
        ImGui::TableNextColumn();
        if (p.host) {
            ImGui::TextColored(ImColor(style::grey()), "HOST");
        } else {
            ImGui::TextColored(ImColor(p.ready ? style::green() : style::yellow()), p.ready ? "READY" : "NOT READY");
            if (v.mode == lobby::Mode::Hosting) {
                ImGui::SameLine();
                if (ImGui::SmallButton(("REMOVE##" + std::to_string(i)).c_str())) {
                    lobby::kick(i);
                }
            }
        }
    }
    ImGui::EndTable();
    text_wrapped("Every player runs the same build and ROM: a machine that does not is turned away with the reason.",
                 style::grey());
}

void settings_box(const lobby::View& v) {
    if (v.settings.empty()) {
        return;
    }
    if (ImGui::CollapsingHeader("HOST'S SETTINGS (everyone plays with these)")) {
        ImGui::TextColored(ImColor(style::grey()), "%s", v.settings.c_str());
    }
    if (!v.settings_differ.empty()) {
        text_wrapped("Differs from this machine's own settings (for this session only):\n" + v.settings_differ, style::yellow());
    }
}

void messages(const lobby::View& v) {
    if (!v.error.empty()) {
        text_wrapped(v.error, style::red());
    }
    if (!v.notice.empty() && v.error.empty()) {  // a notice is older than the error ("Asking...")
        text_wrapped(v.notice, style::green());
    }
}

bool button(const char* label, bool enabled = true) {
    ImGui::BeginDisabled(!enabled);
    const bool pressed = ImGui::Button(label, ImVec2(0, 0));
    ImGui::EndDisabled();
    if (g_focus_first && enabled) {
        ImGui::SetItemDefaultFocus();
        ImGui::SetKeyboardFocusHere(-1);
        ImGui::SetNavCursorVisible(true);  // or a pad's first A only shows the highlight
        g_focus_first = false;
    }
    return pressed;
}

void main_page(const lobby::View& v) {
    ImGui::TextWrapped("Play MULTI with friends over the network. One player hosts; the others join.");
    text_wrapped("When the session starts, the game starts again: progress here that is not saved is lost.", style::grey());
    ImGui::Spacing();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
    if (ImGui::InputText("YOUR NAME", g_name, sizeof(g_name), ImGuiInputTextFlags_EnterReturnsTrue) ||
        ImGui::IsItemDeactivatedAfterEdit()) {
        lobby::set_name(g_name);
        std::snprintf(g_name, sizeof(g_name), "%s", lobby::name().c_str());
    }
    ImGui::Separator();
    if (button("HOST A GAME")) {
        lobby::set_name(g_name);
        lobby::host(conker::net::kDefaultPort);
    }
    ImGui::SameLine();
    ImGui::TextColored(ImColor(style::grey()), "on UDP port %u", conker::net::kDefaultPort);
    ImGui::Separator();
    ImGui::TextUnformatted("JOIN: the host's address or join code");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
    const bool entered = ImGui::InputText("##target", g_target, sizeof(g_target), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("JOIN") || entered) {
        lobby::set_name(g_name);
        lobby::join(g_target);
    }
    ImGui::Spacing();
    ImGui::TextUnformatted("GAMES ON THIS NETWORK");
    if (v.games.empty()) {
        ImGui::TextColored(ImColor(style::grey()), "None found yet (looking...)");
    }
    for (size_t i = 0; i < v.games.size(); i++) {
        const lobby::Game& g = v.games[i];
        ImGui::PushID(static_cast<int>(i));
        const std::string label = g.name + "  " + g.address + "  " + std::to_string(g.players) +
                                  (g.players == 1 ? " player" : " players") + (g.playing ? "  PLAYING" : "  LOBBY");
        if (button(("JOIN##" + std::to_string(i)).c_str(), g.problem.empty())) {
            lobby::set_name(g_name);
            std::snprintf(g_target, sizeof(g_target), "%s", g.address.c_str());
            lobby::join(g.address);
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(label.c_str());
        if (!g.problem.empty()) {
            text_wrapped("    " + g.problem, style::yellow());
        }
        ImGui::PopID();
    }
    ImGui::Spacing();
    messages(v);
}

void hosting_page(const lobby::View& v) {
    ImGui::Text("Hosting on UDP port %u", v.game_port);
    const ImU32 port_color = v.port.state == PortState::Opened ? style::green() : v.port.state == PortState::Failed ? style::yellow() : style::grey();
    text_wrapped(v.port.message, port_color);
    ImGui::Spacing();
    if (!v.codes.empty()) {
        ImGui::TextUnformatted("JOIN CODES (give one to your friends)");
        for (size_t i = 0; i < v.codes.size(); i++) {
            ImGui::PushID(static_cast<int>(100 + i));
            if (ImGui::SmallButton("COPY")) {
                ImGui::SetClipboardText(v.codes[i].second.c_str());
            }
            ImGui::SameLine();
            ImGui::Text("%s   %s", v.codes[i].second.c_str(), v.codes[i].first.c_str());
            ImGui::PopID();
        }
        text_wrapped("A code is this machine's address: whoever joins learns it, as you learn theirs.", style::grey());
    }
    ImGui::Separator();
    players_table(v);
    settings_box(v);
    ImGui::Separator();
    static bool could_start = false;
    if (v.can_start != could_start) {
        could_start = v.can_start;
        g_focus_first = true;  // the pad's highlight to START once it can be pressed, else LEAVE
    }
    if (button("START", v.can_start)) {
        lobby::start();
    }
    ImGui::SameLine();
    if (button("LEAVE")) {
        lobby::leave();
        lobby::browse();
        g_focus_first = true;
    }
    if (!v.can_start && v.players.size() < 2) {
        ImGui::SameLine();
        ImGui::TextColored(ImColor(style::grey()), "Waiting for players...");
    } else if (!v.can_start) {
        ImGui::SameLine();
        ImGui::TextColored(ImColor(style::grey()), "Waiting for everyone to be READY");
    }
    messages(v);
}

void joined_page(const lobby::View& v) {
    players_table(v);
    settings_box(v);
    ImGui::Separator();
    if (button(v.ready ? "NOT READY" : "READY")) {
        lobby::set_ready(!v.ready);
    }
    ImGui::SameLine();
    if (ImGui::Button("LEAVE")) {
        lobby::leave();
        lobby::browse();
        g_focus_first = true;
    }
    ImGui::SameLine();
    ImGui::TextColored(ImColor(style::grey()), v.ready ? "Waiting for the host to START" : "Press READY when you are");
    messages(v);
}

void build() {
    const lobby::View v = lobby::view();
    if (v.mode != g_last_mode) {
        g_last_mode = v.mode;
        g_focus_first = true;
    }
    ImGuiIO& io = ImGui::GetIO();
    const float scale = std::clamp(io.DisplaySize.y / 720.0f, 1.0f, 3.0f);
    static ImFont* styled_font = nullptr;
    if (scale != g_scale || style::font() != styled_font) {
        g_scale = scale;
        styled_font = style::font();
        style::apply(scale);
    }
    const bool wood = style::font() != nullptr;  // the large heading, in place of the title bar
    if (style::font() != nullptr) {
        ImGui::PushFont(style::font(), 20.0f);
    }
    const float width = std::min(io.DisplaySize.x - 40.0f, 760.0f * scale);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.0f), ImVec2(width, io.DisplaySize.y - 40.0f));
    const char* title = v.mode == Mode::Hosting ? "ONLINE - HOSTING###lobby"
                        : v.mode == Mode::Joined ? "ONLINE - IN A LOBBY###lobby"
                                                 : "ONLINE###lobby";
    ImGui::Begin(title, nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_AlwaysAutoResize | (wood ? ImGuiWindowFlags_NoTitleBar : 0));
    if (ImGui::IsWindowAppearing()) {
        ImGui::SetWindowFocus();
    }
    style::panel();
    if (wood) {
        style::heading(v.mode == Mode::Hosting ? "ONLINE - HOSTING" : v.mode == Mode::Joined ? "ONLINE - LOBBY" : "ONLINE");
    }
    if (conker::net::internet_allowed()) {
        text_wrapped("UNSAFE: internet play on. The online code's security is not trusted yet: play only with "
                     "people you know.", style::red());
    } else {
        text_wrapped("LAN only: internet play is off until its security is trusted. It needs --net-unsafe-internet "
                     "on the command line (docs/online-play.md).", style::grey());
    }
    switch (v.mode) {
        case Mode::Idle:
            main_page(v);
            break;
        case Mode::Hosting:
            hosting_page(v);
            break;
        case Mode::Probing:
            text_wrapped(v.notice.empty() ? "Asking the host..." : v.notice);
            if (button("CANCEL")) {
                lobby::leave();
                lobby::browse();
            }
            break;
        case Mode::Joined:
            joined_page(v);
            break;
        case Mode::Starting:
            text_wrapped(v.notice.empty() ? "Starting the session..." : v.notice, style::green());
            break;
    }
    ImGui::Separator();
    style::prompts({{"a", "SELECT"}, {"b", v.mode == Mode::Idle ? "CLOSE" : "LEAVE"}});
    text_wrapped("Playing over the internet (port forwarding, firewalls, overlay networks): docs/online-play.md",
                 style::grey());
    if (v.mode == Mode::Idle && button("CLOSE", true)) {
        close();
    }
    // Back: leave a lobby, or close the window from the first page.
    const bool typing = ImGui::GetIO().WantTextInput;
    if (!typing && (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))) {
        if (v.mode == Mode::Idle) {
            close();
        } else if (v.mode != Mode::Starting) {
            lobby::leave();
            lobby::browse();
        }
    }
    ImGui::End();
    if (style::font() != nullptr) {
        ImGui::PopFont();
    }
}

// RT64's SDL backend for ImGui reads no game controller: give ImGui the first one's buttons and left
// stick for its gamepad navigation (A activates, B is back, the d-pad or stick moves). ImGui moves
// between items with the d-pad only (its left stick scrolls), so the stick pushed past half way
// presses the d-pad that way too.
void feed_pad() {
    ImGuiIO& io = ImGui::GetIO();
    static SDL_GameController* pad = nullptr;
    if (pad != nullptr && !SDL_GameControllerGetAttached(pad)) {
        SDL_GameControllerClose(pad);
        pad = nullptr;
    }
    for (int i = 0; pad == nullptr && i < SDL_NumJoysticks(); i++) {
        if (SDL_IsGameController(i)) {
            pad = SDL_GameControllerOpen(i);
        }
    }
    if (pad == nullptr) {
        io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
        return;
    }
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    static const std::pair<ImGuiKey, SDL_GameControllerButton> buttons[] = {
        {ImGuiKey_GamepadStart, SDL_CONTROLLER_BUTTON_START},
        {ImGuiKey_GamepadBack, SDL_CONTROLLER_BUTTON_BACK},
        {ImGuiKey_GamepadFaceDown, SDL_CONTROLLER_BUTTON_A},
        {ImGuiKey_GamepadFaceRight, SDL_CONTROLLER_BUTTON_B},
        {ImGuiKey_GamepadFaceLeft, SDL_CONTROLLER_BUTTON_X},
        {ImGuiKey_GamepadFaceUp, SDL_CONTROLLER_BUTTON_Y},
        {ImGuiKey_GamepadL1, SDL_CONTROLLER_BUTTON_LEFTSHOULDER},
        {ImGuiKey_GamepadR1, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER},
    };
    for (const auto& [key, button] : buttons) {
        io.AddKeyEvent(key, SDL_GameControllerGetButton(pad, button) != 0);
    }
    const auto stick = [&](ImGuiKey key, SDL_GameControllerAxis axis, float sign) {
        constexpr float kDeadZone = 8000.0f;
        const float value = sign * static_cast<float>(SDL_GameControllerGetAxis(pad, axis));
        const float amount = std::clamp((value - kDeadZone) / (32767.0f - kDeadZone), 0.0f, 1.0f);
        io.AddKeyAnalogEvent(key, amount > 0.1f, amount);
    };
    stick(ImGuiKey_GamepadLStickLeft, SDL_CONTROLLER_AXIS_LEFTX, -1.0f);
    stick(ImGuiKey_GamepadLStickRight, SDL_CONTROLLER_AXIS_LEFTX, 1.0f);
    stick(ImGuiKey_GamepadLStickUp, SDL_CONTROLLER_AXIS_LEFTY, -1.0f);
    stick(ImGuiKey_GamepadLStickDown, SDL_CONTROLLER_AXIS_LEFTY, 1.0f);
    const auto dpad = [&](ImGuiKey key, SDL_GameControllerButton button, SDL_GameControllerAxis axis, float sign) {
        constexpr Sint16 kHalfWay = 16384;
        const bool pushed = sign * static_cast<float>(SDL_GameControllerGetAxis(pad, axis)) > kHalfWay;
        io.AddKeyEvent(key, SDL_GameControllerGetButton(pad, button) != 0 || pushed);
    };
    dpad(ImGuiKey_GamepadDpadLeft, SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_AXIS_LEFTX, -1.0f);
    dpad(ImGuiKey_GamepadDpadRight, SDL_CONTROLLER_BUTTON_DPAD_RIGHT, SDL_CONTROLLER_AXIS_LEFTX, 1.0f);
    dpad(ImGuiKey_GamepadDpadUp, SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_AXIS_LEFTY, -1.0f);
    dpad(ImGuiKey_GamepadDpadDown, SDL_CONTROLLER_BUTTON_DPAD_DOWN, SDL_CONTROLLER_AXIS_LEFTY, 1.0f);
}

// RT64's present thread: the window over the frame.
void draw_hook(RenderCommandList* list, RenderFramebuffer* /*framebuffer*/) {
    if (!g_open.load() || !g_frame_ready.load() || g_imgui == nullptr) {
        return;
    }
    g_imgui->draw(list);
}

}  // namespace

void set_window(SDL_Window* window) {
    g_window = window;
    load_script();
    if (conker::net::lobby_at_boot()) {
        open();
    }
}

void attach(RT64::Application* app) {
    RT64::SetRenderHooks(nullptr, &draw_hook, nullptr);
    g_app.store(app);
}

void open() {
    if (conker::net::active()) {
        return;  // a session is already on
    }
    if (!g_open.exchange(true)) {
        if (g_opened_at == 0) {
            g_opened_at = ticks();
        }
        std::snprintf(g_name, sizeof(g_name), "%s", lobby::name().c_str());
        if (g_target[0] == '\0') {
            std::snprintf(g_target, sizeof(g_target), "%s", lobby::last_join().c_str());
        }
        g_focus_first = true;
        conker::input::set_blocked(true);
        lobby::browse();
        std::printf("[conker] lobby: window open\n");
    }
}

void close() {
    if (g_open.exchange(false)) {
        g_frame_ready.store(false);
        if (lobby::view().mode != Mode::Starting) {
            lobby::leave();
        }
        conker::input::set_blocked(false);
        std::printf("[conker] lobby: window closed\n");
    }
}

bool is_open() { return g_open.load(); }

bool handle_event(const SDL_Event& event) {
    if (!g_open.load() || g_imgui == nullptr) {
        return false;
    }
    ImGui_ImplSDL2_ProcessEvent(&event);
    return event.type == SDL_KEYDOWN || event.type == SDL_KEYUP || event.type == SDL_TEXTINPUT ||
           event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_MOUSEBUTTONUP || event.type == SDL_MOUSEWHEEL;
}

void update() {
    if (!g_script.empty() && g_opened_at != 0) {
        run_script();
    }
    if (!g_open.load()) {
        return;
    }
    const uint64_t now = ticks();
    if (now - g_last_frame < 16 || !make_imgui()) {
        return;
    }
    g_last_frame = now;
    feed_pad();
    style::prepare(g_app.load()->core.RDRAM);
    g_imgui->newFrame(g_app.load()->framebufferGraphicsWorker.get());
    build();
    g_imgui->endFrame();
    g_frame_ready.store(g_open.load());
}

}  // namespace conker::lobby_ui
