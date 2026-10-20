// The debug menu: the pad's Back button (Create/Share on a PlayStation pad, View on an Xbox pad)
// or the ` key opens it during play. Native helpers called from the [[patches.hook]] snippets in
// config/us.toml (func_151E51EC, func_151DDC20).
//
// It is the game's own pause with its page replaced, so the game stops just as it does for the
// pause menu and the menu draws with the game's font. A press asks for the pause: at the start of
// func_151DDC20, the in-play frame handler, Start is added to controller 1's buttons pressed this
// frame (D_800BE930), so the pause opens only where Start would open it, and taken out again once
// the handler has looked for it. When that pause opened (D_800BEAC1), the pause page
// (0x151DE00C on) draws this menu and skips to the end of the frame, 0x151DE698, which plays the
// sound left in the frame's sound slot. Closing it is CONT...'s: the close countdown in
// D_8008FEEC/F0.
//
//     WIREFRAME        the game's 3D triangles drawn as their edges (src/platform/render_debug.hpp)
//     SHOW FPS         the frames the game draws per second, in the corner, on every screen
//     LOAD SNAPSHOT    a list of saves/snapshots/; A starts one (resume_point.cpp)
//
// Up and down choose, A or left and right change, B goes back, and Back, Start or B on the first
// page close it. The pad is read directly (conker::input::read), not through the game.
// CONKER_DEBUG_MENU=0 turns it off.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "recomp.h"
#include "enhancements/pause_save/resume_point.hpp"
#include "platform/input.hpp"
#include "platform/render_debug.hpp"

extern "C" void func_15042D94(uint8_t* rdram, recomp_context* ctx);

namespace {

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
// The pause keeps drawing PAUSED above and the HUD below, so the menu fits between them.
constexpr int kTitleY = 78;
constexpr int kFirstRowY = 96;
constexpr int kRowHeight = 14;
constexpr int kListRowHeight = 12;
constexpr int kListRows = 4;

using Clock = std::chrono::steady_clock;

enum class Page { Main, Snapshots };
enum Row { kWireframe, kShowFps, kLoadSnapshot, kRowCount };

bool g_open_requested = false;
bool g_injected = false;
uint16_t g_pad_before = 0;
bool g_active = false;
bool g_close_requested = false;
Page g_page = Page::Main;
int g_row = 0;
int g_entry = 0;
int g_top = 0;
std::vector<conker::resume::Snapshot> g_snapshots;
std::string g_message;
Clock::time_point g_message_until;

bool g_wireframe = false;
bool g_show_fps = false;
int g_frames = 0;
Clock::time_point g_fps_since = Clock::now();
int g_fps = 0;

uint16_t g_previous_buttons = 0;
int g_previous_direction = 0;
Clock::time_point g_repeat_at;

bool debug_menu_enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("CONKER_DEBUG_MENU");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
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

void draw_main(uint8_t* rdram, recomp_context* ctx) {
    draw_text(rdram, ctx, kCenterX, kTitleY, kCentered, kTitleColor, "DEBUG MENU");
    const std::string rows[kRowCount] = {
        std::string("WIREFRAME  ") + on_off(g_wireframe),
        std::string("SHOW FPS  ") + on_off(g_show_fps),
        "LOAD SNAPSHOT",
    };
    for (int row = 0; row < kRowCount; row++) {
        draw_text(rdram, ctx, kCenterX, kFirstRowY + row * kRowHeight, kCentered, row == g_row ? kChosenColor : kOtherColor,
                  rows[row]);
    }
}

void draw_snapshots(uint8_t* rdram, recomp_context* ctx) {
    const int count = static_cast<int>(g_snapshots.size());
    char title[kTextSize];
    if (!g_message.empty() && Clock::now() < g_message_until) {
        std::snprintf(title, sizeof(title), "%s", g_message.c_str());
    } else if (count > 0) {
        std::snprintf(title, sizeof(title), "LOAD SNAPSHOT  %d/%d", g_entry + 1, count);
    } else {
        std::snprintf(title, sizeof(title), "LOAD SNAPSHOT");
    }
    draw_text(rdram, ctx, kCenterX, kTitleY, kCentered, kTitleColor, title);
    if (g_snapshots.empty()) {
        draw_text(rdram, ctx, kCenterX, kFirstRowY, kCentered, kOtherColor, "NO SNAPSHOTS YET");
        draw_text(rdram, ctx, kCenterX, kFirstRowY + kListRowHeight, kCentered, kOtherColor, "SAVE FROM THE PAUSE MENU");
    }
    for (int i = g_top; i < std::min(count, g_top + kListRows); i++) {
        const conker::resume::Snapshot& snapshot = g_snapshots[i];
        char line[kTextSize];
        std::snprintf(line, sizeof(line), "SCENE %-3d %s", snapshot.scene, snapshot.when.c_str());
        draw_text(rdram, ctx, kCenterX, kFirstRowY - 2 + (i - g_top) * kListRowHeight, kCentered,
                  i == g_entry ? kChosenColor : kOtherColor, line);
    }
}

// --- input ---------------------------------------------------------------------------------------

// -1 up, 1 down, 0 neither: a fresh push, then repeats while held.
int vertical_step(uint16_t buttons, float y) {
    int direction = 0;
    if ((buttons & conker::input::kDUp) || y > 0.5f) direction = -1;
    if ((buttons & conker::input::kDDown) || y < -0.5f) direction = 1;
    const Clock::time_point now = Clock::now();
    int step = 0;
    if (direction != 0 && direction != g_previous_direction) {
        step = direction;
        g_repeat_at = now + std::chrono::milliseconds(400);
    } else if (direction != 0 && now >= g_repeat_at) {
        step = direction;
        g_repeat_at = now + std::chrono::milliseconds(90);
    }
    g_previous_direction = direction;
    return step;
}

// A fresh push left or right.
bool horizontal_push(uint16_t buttons, float x) {
    static int previous = 0;
    const int direction = ((buttons & conker::input::kDLeft) || x < -0.5f)    ? -1
                          : ((buttons & conker::input::kDRight) || x > 0.5f) ? 1
                                                                              : 0;
    const bool push = direction != 0 && direction != previous;
    previous = direction;
    return push;
}

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

}  // namespace

// In func_151E51EC, the menu handler, once a frame on every screen (after func_15042D78): counts
// frames, draws the frame rate, and takes a press of the debug menu button.
extern "C" void conker_debug_menu_tick(uint8_t* rdram, recomp_context* ctx) {
    g_frames++;
    const Clock::time_point now = Clock::now();
    const double elapsed = std::chrono::duration<double>(now - g_fps_since).count();
    if (elapsed >= 0.5) {
        g_fps = static_cast<int>(g_frames / elapsed + 0.5);
        g_frames = 0;
        g_fps_since = now;
    }
    if (g_show_fps) {
        draw_text(rdram, ctx, 16, 12, kLeft, 0xFFFFFFFF, std::to_string(g_fps) + " FPS");
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
        uint16_t buttons;
        float x, y;
        conker::input::read(&buttons, &x, &y);
        g_previous_buttons = buttons;  // a button held while opening does not count as a press
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
    const int step = vertical_step(buttons, y);
    const bool push = horizontal_push(buttons, x);
    int32_t sound = 0;

    if (g_close_requested || (pressed & conker::input::kStart) ||
        (g_page == Page::Main && (pressed & conker::input::kB))) {
        close_menu(rdram);
        MEM_W(kSoundSlot, ctx->r29) = kSelectSound;
        return;
    }

    if (g_page == Page::Main) {
        if (step != 0) {
            g_row = (g_row + step + kRowCount) % kRowCount;
            sound = kMoveSound;
        }
        const bool activate = (pressed & conker::input::kA) != 0;
        if (g_row == kWireframe && (activate || push)) {
            set_wireframe(!g_wireframe);
            sound = kSelectSound;
        } else if (g_row == kShowFps && (activate || push)) {
            g_show_fps = !g_show_fps;
            std::printf("[conker] debug menu: show fps %s\n", g_show_fps ? "on" : "off");
            sound = kSelectSound;
        } else if (g_row == kLoadSnapshot && activate) {
            g_snapshots = conker::resume::list_snapshots();
            g_page = Page::Snapshots;
            g_entry = 0;
            g_top = 0;
            g_message.clear();
            sound = kSelectSound;
        }
    } else {
        const int count = static_cast<int>(g_snapshots.size());
        if (pressed & conker::input::kB) {
            g_page = Page::Main;
            sound = kSelectSound;
        } else if (step != 0 && count > 0) {
            g_entry = std::clamp(g_entry + step, 0, count - 1);
            g_top = std::clamp(g_top, g_entry - kListRows + 1, g_entry);
            sound = kMoveSound;
        } else if ((pressed & conker::input::kA) && count > 0) {
            if (conker::resume::load_snapshot(rdram, g_snapshots[g_entry].path)) {
                close_menu(rdram);
            } else {
                g_message = "SAVING, TRY AGAIN";
                g_message_until = Clock::now() + std::chrono::seconds(2);
            }
            sound = kSelectSound;
        }
    }

    if (g_page == Page::Main) {
        draw_main(rdram, ctx);
    } else {
        draw_snapshots(rdram, ctx);
    }
    if (sound != 0) {
        MEM_W(kSoundSlot, ctx->r29) = sound;
    }
}
