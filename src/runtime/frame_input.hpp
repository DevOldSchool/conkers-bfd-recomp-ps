// The game's input, one record per game frame. Once a frame,
// at the start of func_1501905C (before func_1501BBB8 reads the pads), every port's pad, the
// scripted presses and controller 1's mouse are sampled into a record, and everything the game
// logic sees of the input for that frame comes from it: the PIF shim's controller answers and
// rumble pak status (src/runtime/ultra_extras.cpp), osContGetReadData (src/main.cpp), the right
// stick and mouse of consistent controls and mouse look. So the game reads its input in one place a
// frame, which a replay or the network can then supply. Before the first frame (boot) the live
// input is read instead.
//
// CONKER_FIXED_STEP=1 also makes every frame step the logic by the same amount (2 retraces, or 1
// at 60 fps) instead of the retraces the frame took, so a run depends on its input and not on how
// fast the machine is. CONKER_FRAME_INPUT_LOG=<file> writes each frame's record as a line, and
// CONKER_FRAME_INPUT_REPLAY=<file> plays such a log back in place of the pads, frame by frame.
#pragma once

#include <chrono>
#include <cstdint>
#include <string>

#include "recomp.h"
#include "platform/input.hpp"

namespace conker::frame_input {

struct Port {
    static constexpr uint8_t kConnected = 0x01;
    static constexpr uint8_t kRightStick = 0x02;  // a pad holds the port, so it has a right stick
    static constexpr uint8_t kRumble = 0x04;
    static constexpr uint8_t kInvertAim = 0x08;  // the player's INVERT AIM (invert_aim below)
    // The player's CONSISTENT CONTROLS and camera swap: online each player's own, applied to their port
    // on every machine alike (src/enhancements/consistent_controls).
    static constexpr uint8_t kConsistentControls = 0x10;
    static constexpr uint8_t kCameraSwap = 0x20;

    uint16_t buttons = 0;  // N64 bits (conker::input::kA...), scripted presses included
    int8_t stick_x = 0;    // as the PIF answers it: about +-80, +y up
    int8_t stick_y = 0;
    int8_t right_x = 0;    // [-1, 1] in steps of 1/127, +y up
    int8_t right_y = 0;
    uint8_t flags = 0;
};

// A port's mouse (and zoom keys), since the frame before. On one machine only controller 1 has
// one; online each machine's goes with the port its controller 1 holds.
struct Mouse {
    float dx = 0.0f;  // pixels, +x right, +y down
    float dy = 0.0f;
    int8_t zoom = 0;  // steps, + in
    bool captured = false;
};

struct Record {
    uint32_t frame = 0;  // 1 for the first game frame
    Port ports[input::kPorts];
    Mouse mice[input::kPorts];
};

// Whether CONKER_FIXED_STEP=1, or a net session (src/net/net_play.hpp), which forces it.
bool fixed_step();

// A clock for game code that times things: the real one, or with the fixed step the game frames
// so far at 30 (or 60) a second, so what it times lands on the same frame in every run.
std::chrono::steady_clock::time_point clock_now();

// Called by the renderer when it has finished a graphics task's display list (colour and depth
// written back to RDRAM). With the fixed step each game frame starts only once every task the game
// has sent is finished, so the frame's logic sees the frame before's depth buffer in every run.
void gfx_task_done();

// The record the game is reading, safe from any thread; the live input until the first frame.
Record current();

// A port of the current record (an empty one outside 0 to 3).
Port port(int port);

// The same calls as conker::input's, answered from the current record.
void read(uint16_t* buttons, float* x, float* y, int port);
bool right_stick(float* x, float* y, int port);
bool connected(int port);
bool has_rumble(int port);
bool mouse_captured(int port);
// The port's mouse motion this frame, the first time it is asked for in a frame; nothing after that.
void take_mouse(int port, float* dx, float* dy, int* zoom);

// How fast the mouse aims, as a percentage: the CONTROLS page's MOUSE AIM row
// (src/enhancements/debug_menu/debug_menu.cpp), kept in conker-recomp-data/input.cfg.
// It scales the motion as it goes into the record while the mouse aims; the camera is not scaled.
constexpr int kMinMouseSpeed = 25;
constexpr int kMaxMouseSpeed = 400;
int mouse_speed();
void set_mouse_speed(int percent);

// The debug menu's CONTROLS page, kept in input.cfg too. INVERT AIM, per port, turns the stick's aim
// the other way (pushed up aims down, as on the N64) wherever consistent controls aim it; it goes
// into the record as Port::kInvertAim. INVERT MOUSE AIM turns controller 1's mouse's aim the other
// way, as its motion goes into the record.
bool invert_aim(int port);
void set_invert_aim(int port, bool on);
bool invert_mouse_aim();
void set_invert_mouse_aim(bool on);

// The settings a run depends on besides its input, as "# " header lines: the build, the fixed step
// and every switch ("# switch LABEL=0|1"). A replay log starts with them; online the host sends
// them.
std::string settings_header();

// Sets the switches as another machine's header lines (an online host's) have them, but for this
// machine's own (Switch::local); source names where they came from in messages.
void apply_settings(const std::string& header, const char* source);

// With the fixed step, makes the audio frame on the game thread at the start of the game frame
// (src/runtime/frame_audio.cpp); hook_ctx is the frame hook's registers.
void make_audio_frame(uint8_t* rdram, recomp_context* hook_ctx);

// A session the online lobby starts turns the fixed step on while the game runs, and off when the
// player leaves it (src/net/net_play.cpp). On the game thread at the start of a frame, before the
// frame waits for its graphics: from then on the frames step evenly, osGetCount counts frames, and
// the game thread makes the audio frames instead of Rare's audio thread (take_audio_frames). False,
// changing nothing, while the audio thread is not waiting at the top of its loop (the next frame
// tries again).
bool enter_fixed_step(uint8_t* rdram);
void leave_fixed_step();
// The audio frames from Rare's audio thread to the game thread's model, carrying on where the thread
// was (frame_audio.cpp); false while it is not at the top of its loop.
bool take_audio_frames(uint8_t* rdram);
// And back to the audio thread on leaving a session, after this machine's own game is put back.
void give_back_audio_frames(uint8_t* rdram);
// The frame number the next frame's record follows: a session's frames count from 1.
void set_frame(uint32_t frame);
// After a session: this machine's own settings (settings_header() before it) back, its own
// (Switch::local) ones too.
void restore_settings(const std::string& header);

// Under the fixed step, how many times the game thread has read osGetCount this frame (each read
// is a step on); saved and put back by a pass that runs game code it must not count (my_view).
uint32_t count_reads();
void set_count_reads(uint32_t reads);

}  // namespace conker::frame_input
