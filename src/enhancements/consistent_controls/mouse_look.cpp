// The mouse in the game, a mouse a port: on one machine controller 1's (src/platform/input_sdl.cpp
// captures it and gathers its motion), online each machine's for the port it holds. Each player's
// camera follows the mouse of the port that player reads. Moved left and right, it turns the third-person camera itself, not through the C
// buttons, so the view follows the mouse; while aiming (the shotgun, the rocket launcher's scope, R's
// first-person look; consistent_controls.cpp) it is a stick deflection for this frame instead, as
// the game aims at the speed the stick asks. The wheel, or R and T, step the camera in and out
// through the game's three distances, both ways, where C-up only steps in.
// CONKER_MOUSE_SENSITIVITY scales both (1 by default), applied as the motion goes into the frame's
// input record (src/runtime/frame_input.cpp).
//
// The camera: the controls block (*(player + 0x318)) turns with C-left and C-right in
// func_1512D390, which builds a swing speed (+0x6C4, up to 10) and turns the eye (+0x2F8) about
// the pivot (+0x2A4) by speed * tick / 2.5 degrees through func_1508EF80, C-right one way and
// C-left the other; it does nothing while +0x84 has bit 21 or +0x698 is set. The mouse turns the
// eye the same way at that function's start ([[patches.hook]] in config/us.toml): func_1508EF80
// takes the angle in degrees (times D_8009DC80, to radians), and turns the offset from the pivot
// x' = c*x + s*z, z' = c*z - s*x.
//
// The zoom: func_15123568 steps the index +0x1B4 (3 far, 2, 1 near; only those with their bit in
// +0x1E0) towards 1 on C-up when +0x84 has bit 2, +0xDC is not 4, no camera zone holds it (+0x6C8 0
// or +0x6FC 4) and its cooldown +0x7B8 has run out, then calls func_15124B18 and func_15125608 and
// sets the cooldown to 20 ticks. The wheel does the same at that function's start, either way.
//
// A camera's controls block is *(actor + 0x318) of its player's actor (Conker's, D_80088720, for the
// first; in a match player n is the actor at 0x800CC2D0 + n * 0x32C and reads pad D_80084060[n]).
#include "enhancements/consistent_controls/mouse_look.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "recomp.h"
#include "enhancements/object_inspector/object_inspector.hpp"
#include "platform/input.hpp"
#include "runtime/frame_input.hpp"
#include "runtime/snapshot.hpp"

extern "C" void func_15124B18(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_15125608(uint8_t* rdram, recomp_context* ctx);

namespace {

inline gpr kseg0(uint32_t address) { return static_cast<gpr>(static_cast<int32_t>(address)); }

constexpr uint32_t kMenuHandler = 0x800E0B94;   // D_800E0B94: the frame's screen handler, 0 in play
constexpr uint32_t kPauseOpen = 0x800BEAC1;     // D_800BEAC1: the pause (and the debug menu) is up
constexpr uint32_t kPauseRunning = 0x800BEAC0;  // D_800BEAC0
constexpr uint32_t kDegreesToRadians = 0x8009DC80;  // D_8009DC80, func_1508EF80's factor

constexpr uint32_t kCameraFlags = 0x84;
constexpr uint32_t kCameraFixed = 0x698;
constexpr uint32_t kCameraEye = 0x2F8;    // 3 floats
constexpr uint32_t kCameraPivot = 0x2A4;  // 3 floats
constexpr uint32_t kZoomIndex = 0x1B4;    // s16
constexpr uint32_t kZoomMask = 0x1E0;     // s16
constexpr uint32_t kZoomCooldown = 0x7B8;  // s32
constexpr uint32_t kZoomFrozen = 0x3D4;   // a pointer; its u8 +0x120 at 4 holds the zoom near

constexpr uint32_t kConker = 0x80088720;       // D_80088720: Conker's actor
constexpr uint32_t kActors = 0x800CC2D0;       // D_800CC2D0: player n's actor in a match
constexpr uint32_t kActorSize = 0x32C;
constexpr uint32_t kActorCamera = 0x318;       // the actor's camera controls block
constexpr uint32_t kMultiplayer = 0x800BE616;  // D_800BE616: nonzero during a match
constexpr uint32_t kPlayerPads = 0x80084060;   // D_80084060: u8 per player, the pad it reads
constexpr int kPorts = conker::input::kPorts;

constexpr float kDegreesPerPixel = 0.15f;  // the camera
constexpr float kStickPerPixel = 0.05f;    // aiming: 20 pixels in a frame is the stick pushed fully

// By port.
bool g_captured[kPorts] = {};
float g_dx[kPorts] = {};  // this frame's motion not yet used, pixels
float g_dy[kPorts] = {};
int g_zoom[kPorts] = {};  // steps waiting, + in
// Native state the game's memory depends on, carried by a snapshot (runtime/snapshot.hpp).
const bool g_snapshot_fields = [] {
    // Checked on load, as another machine's word.
    const auto finite = [](const uint8_t* bytes) {
        for (int i = 0; i < kPorts; i++) {
            float v;
            std::memcpy(&v, bytes + i * sizeof(float), sizeof(v));
            if (!std::isfinite(v)) return false;
        }
        return true;
    };
    conker::snapshot::add_bools("mouse_look.captured", g_captured, kPorts);
    conker::snapshot::add("mouse_look.dx", g_dx, sizeof(g_dx), finite);
    conker::snapshot::add("mouse_look.dy", g_dy, sizeof(g_dy), finite);
    conker::snapshot::add("mouse_look.zoom", g_zoom, sizeof(g_zoom), [](const uint8_t* bytes) {
        for (int i = 0; i < kPorts; i++) {
            int v;
            std::memcpy(&v, bytes + i * sizeof(int), sizeof(v));
            if (v < -2 || v > 2) return false;
        }
        return true;
    });
    return true;
}();

float read_float(uint8_t* rdram, uint32_t address) {
    const uint32_t word = static_cast<uint32_t>(MEM_W(0, kseg0(address)));
    float value;
    std::memcpy(&value, &word, sizeof(value));
    return value;
}

void write_float(uint8_t* rdram, uint32_t address, float value) {
    uint32_t word;
    std::memcpy(&word, &value, sizeof(word));
    MEM_W(0, kseg0(address)) = static_cast<int32_t>(word);
}

// Calls one of the game's functions with a0 from a hook, leaving the hook's registers as they were.
void call_game(uint8_t* rdram, recomp_context* ctx, void (*function)(uint8_t*, recomp_context*), uint32_t a0) {
    recomp_context saved = *ctx;
    ctx->r29 -= 0x40;
    ctx->r4 = kseg0(a0);
    function(rdram, ctx);
    *ctx = saved;
}

// The port whose mouse turns this camera controls block: its player's pad; controller 1 outside a
// match, or when the block is no player's.
int camera_port(uint8_t* rdram, uint32_t camera) {
    if (MEM_BU(0, kseg0(kMultiplayer)) == 0) {
        return 0;
    }
    for (int player = 0; player < kPorts; player++) {
        const uint32_t actor = player == 0 ? static_cast<uint32_t>(MEM_W(0, kseg0(kConker))) : kActors + player * kActorSize;
        if (actor < 0x80000000u || actor >= 0x80800000u) {
            continue;
        }
        if (static_cast<uint32_t>(MEM_W(kActorCamera, kseg0(actor))) == camera) {
            const uint8_t pad = MEM_BU(player, kseg0(kPlayerPads));
            return pad < kPorts ? pad : -1;
        }
    }
    return 0;
}

}  // namespace

namespace conker::mouse_look {

void frame(uint8_t* rdram) {
    const bool playing = MEM_BU(0, kseg0(kMenuHandler)) == 0 && MEM_BU(0, kseg0(kPauseOpen)) == 0 &&
                         MEM_BU(0, kseg0(kPauseRunning)) == 0 && !conker::inspector::active();
    conker::input::set_mouse_play(playing);
    for (int port = 0; port < kPorts; port++) {
        int zoom;
        conker::frame_input::take_mouse(port, &g_dx[port], &g_dy[port], &zoom);
        g_captured[port] = conker::frame_input::mouse_captured(port);
        if (!g_captured[port] || !playing) {
            g_dx[port] = 0.0f;
            g_dy[port] = 0.0f;
            g_zoom[port] = 0;
            continue;
        }
        g_zoom[port] = std::clamp(g_zoom[port] + zoom, -2, 2);
    }
}

bool take_aim(int port, float* x, float* y) {
    *x = 0.0f;
    *y = 0.0f;
    if (port < 0 || port >= kPorts || !g_captured[port]) {
        return false;
    }
    *x = g_dx[port] * kStickPerPixel;
    *y = -g_dy[port] * kStickPerPixel;
    g_dx[port] = 0.0f;
    g_dy[port] = 0.0f;
    return true;
}

}  // namespace conker::mouse_look

// At func_1512D390's start, $a0 the camera controls block: the mouse turns the eye about the pivot.
extern "C" void conker_mouse_camera(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t camera = static_cast<uint32_t>(ctx->r4);
    const int port = camera_port(rdram, camera);
    if (port < 0 || g_dx[port] == 0.0f) {
        return;
    }
    const float dx = g_dx[port];
    g_dx[port] = 0.0f;
    if ((MEM_W(kCameraFlags, kseg0(camera)) & 0x200000) != 0 || MEM_W(kCameraFixed, kseg0(camera)) != 0) {
        return;
    }
    // Moved right, the view turns right: C-left's way, a negative angle.
    const float angle = -dx * kDegreesPerPixel * read_float(rdram, kDegreesToRadians);
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    const float x = read_float(rdram, camera + kCameraEye) - read_float(rdram, camera + kCameraPivot);
    const float z = read_float(rdram, camera + kCameraEye + 8) - read_float(rdram, camera + kCameraPivot + 8);
    write_float(rdram, camera + kCameraEye, c * x + s * z + read_float(rdram, camera + kCameraPivot));
    write_float(rdram, camera + kCameraEye + 8, c * z - s * x + read_float(rdram, camera + kCameraPivot + 8));
}

// At func_15123568's start, $a0 the camera controls block: one zoom step for the wheel (or R/T).
extern "C" void conker_mouse_zoom(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t camera = static_cast<uint32_t>(ctx->r4);
    const int port = camera_port(rdram, camera);
    if (port < 0 || g_zoom[port] == 0) {
        return;
    }
    int& zoom = g_zoom[port];
    const uint32_t frozen = static_cast<uint32_t>(MEM_W(kZoomFrozen, kseg0(camera)));
    if (frozen != 0 && MEM_BU(0x120, kseg0(frozen)) == 4) {
        zoom = 0;
        return;
    }
    if (MEM_W(kZoomCooldown, kseg0(camera)) > 0) {
        return;  // the step waits for it
    }
    const bool zone = MEM_W(0x6C8, kseg0(camera)) != 0 && MEM_W(0x6FC, kseg0(camera)) != 4;
    if ((MEM_W(kCameraFlags, kseg0(camera)) & 4) == 0 || MEM_W(0xDC, kseg0(camera)) == 4 || zone) {
        zoom = 0;
        return;
    }
    const int step = zoom > 0 ? -1 : 1;  // in is towards index 1
    zoom += zoom > 0 ? -1 : 1;
    const int from = MEM_H(kZoomIndex, kseg0(camera));
    const int mask = MEM_H(kZoomMask, kseg0(camera));
    int index = from + step;
    while (index >= 1 && index <= 3 && (mask & (1 << index)) == 0) {
        index += step;
    }
    if (index < 1 || index > 3) {
        return;
    }
    MEM_H(kZoomIndex, kseg0(camera)) = static_cast<int16_t>(index);
    call_game(rdram, ctx, func_15124B18, camera);
    call_game(rdram, ctx, func_15125608, camera);
    MEM_W(kZoomCooldown, kseg0(camera)) = 0x14;
}
