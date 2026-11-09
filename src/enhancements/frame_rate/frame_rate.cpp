// Experimental 60 fps: CONKER_60FPS=1 lets Rare's scheduler start a frame on every retrace instead
// of every second one. Off by default. Native helpers
// called from the [[patches.hook]] snippets for func_1501C880, func_800049E0, func_80004DB0,
// func_80005020, the half steps in func_15116BAC, func_151169B4, func_151934B4 and func_8000BF60,
// the frame counters in func_15043384 and func_151934B4, the menu repeat timers in func_151DE8F0,
// the full steps in func_1508F9F4, the Doppler shift in func_80011624 and the heading easing in
// func_1501B320, and the random chances, the swarm and the emitter below, in config/us.toml.
//
// The game logic already steps by the retraces its frame took: func_80004F00 copies the count since
// the last frame started (D_8003B238) to D_800BE9E4, and the logic scales by it (D_800BE9A4 =
// count * 0.5). What holds it to 30 fps is the scheduler thread (func_800049E0), which looks at the
// game's task queue only on a retrace. A recompiled frame takes about a millisecond, so it is done
// before the next retrace, waits for it to swap (state 6), and the frame after it is then picked up
// one retrace later still: always 2 retraces. Here the scheduler is woken when a frame is swapped
// and when the game queues its next one, so the next frame starts in the retrace its predecessor
// was swapped in, and the count reads 1.
//
// CONKER_FRAME_LOG=<file> (either mode) writes a line per frame: VI, the frame's retrace count,
// scene, logic delta (D_800BE9A4), Conker's position and the camera's eye, for comparing the two
// rates.
#include <atomic>
#include "platform/data_folder.hpp"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

#include "recomp.h"
#include "ultramodern/ultra64.h"
#include "enhancements/frame_rate/frame_rate.hpp"
#include "enhancements/frame_rate/latency.hpp"
#include "enhancements/status.hpp"
#include "enhancements/switches.hpp"
#include "runtime/game.hpp"

extern "C" void func_80004DB0(uint8_t* rdram, recomp_context* ctx);

namespace {

inline gpr kseg0(uint32_t address) { return static_cast<gpr>(static_cast<int32_t>(address)); }

constexpr uint32_t kSchedulerQueue = 0x8003B218;  // D_8003B218: the scheduler thread's messages
constexpr uint32_t kAudioTaskRunning = 0x8003A581;  // D_8003A581
// func_800049E0's jump table covers messages 0 to 6 and ignores the rest; 7 is free.
constexpr int32_t kWakeMessage = 7;
constexpr int kMessageSlot = 0x64;  // func_800049E0 receives into sp+0x64
constexpr uint32_t kFrameTicks = 0x800BE9E4;    // D_800BE9E4: retraces this frame steps by
constexpr uint32_t kHalfSteps = 0x800BE9A0;     // D_800BE9A0: 30 Hz steps this frame (u8), 0 or 1 at 60 fps
constexpr uint32_t kFrameDelta = 0x800BE9A4;    // D_800BE9A4: the same as a float, 1.0 = 1/30 s
constexpr uint32_t kScene = 0x800BE9F0;         // D_800BE9F0
constexpr uint32_t kPlayerObject = 0x80088720;  // D_80088720: Conker's object, position at +0x14

// --- settings: the OPTIONS page's FRAME RATE row, kept in conker-recomp-data/frame_rate.cfg --------
// "fps=<30|60>" and "smooth=<0|1>"; the original 30 fps without smoothing unless the file says
// otherwise (not every machine holds 60).
// CONKER_60FPS and CONKER_SMOOTH (0 or 1) override them for one run, and CONKER_REFRESH (see the
// renderer) turns smoothing on. The debug menu's switches change them until the game is closed;
// only the row saves them.
struct Settings {
    std::atomic<bool> sixty{false};
    std::atomic<bool> smooth{false};
};

std::mutex g_save_mutex;

std::filesystem::path config_path() {
    return conker::data_folder() / "frame_rate.cfg";
}

bool env_flag(const char* name, bool* value) {
    const char* text = std::getenv(name);
    if (text == nullptr || text[0] == '\0') {
        return false;
    }
    *value = text[0] == '1';
    return true;
}

// A function-local static: the first call (in play, after main has set the data folder) loads it.
Settings& settings() {
    static Settings* loaded = [] {
        auto* result = new Settings;
        std::ifstream file(config_path());
        for (std::string line; std::getline(file, line);) {
            const size_t equals = line.find('=');
            if (equals == std::string::npos) {
                continue;
            }
            const std::string key = line.substr(0, equals);
            const std::string value = line.substr(equals + 1);
            if (key == "fps") {
                result->sixty.store(value == "60");
            } else if (key == "smooth") {
                result->smooth.store(value == "1");
            }
        }
        bool value;
        if (env_flag("CONKER_60FPS", &value)) {
            result->sixty.store(value);
        }
        if (env_flag("CONKER_SMOOTH", &value)) {
            result->smooth.store(value);
        }
        if (const char* refresh = std::getenv("CONKER_REFRESH"); refresh != nullptr && refresh[0] != '\0') {
            result->smooth.store(true);
        }
        return result;
    }();
    return *loaded;
}

void save_settings() {
    std::lock_guard lock(g_save_mutex);
    std::ofstream file(config_path());
    file << "fps=" << (settings().sixty.load() ? 60 : 30) << '\n'
         << "smooth=" << (settings().smooth.load() ? 1 : 0) << '\n';
    if (!file) {
        std::fprintf(stderr, "[conker] could not write %s\n", config_path().string().c_str());
    }
}

bool enabled() { return settings().sixty.load(std::memory_order_relaxed); }

void wake_scheduler(uint8_t* rdram) {
    osSendMesg(rdram, static_cast<int32_t>(kSchedulerQueue), kWakeMessage, OS_MESG_NOBLOCK);
}

float read_float(uint8_t* rdram, uint32_t address) {
    const int32_t bits = MEM_W(0, kseg0(address));
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void log_frame(uint8_t* rdram) {
    static FILE* file = [] {
        const char* path = std::getenv("CONKER_FRAME_LOG");
        FILE* opened = path != nullptr && path[0] != '\0' ? std::fopen(path, "w") : nullptr;
        if (opened != nullptr) {
            // Line buffered, as runs end by being killed. Windows' C runtime refuses a size of 0 (the
            // game stops: an invalid parameter) and buffers _IOLBF fully, so each line is flushed there.
            std::setvbuf(opened, nullptr, _IOLBF, BUFSIZ);
        }
        return opened;
    }();
    if (file == nullptr) {
        return;
    }
    const uint32_t object = static_cast<uint32_t>(MEM_W(0, kseg0(kPlayerObject)));
    float position[3] = {}, eye[3] = {};
    if (object >= 0x80000000u && object < 0x80800000u) {
        const uint32_t camera = static_cast<uint32_t>(MEM_W(0, kseg0(object + 0x31C)));
        for (int i = 0; i < 3; i++) {
            position[i] = read_float(rdram, object + 0x14 + 4 * i);
            if (camera >= 0x80000000u && camera < 0x80800000u) {
                eye[i] = read_float(rdram, camera + 0x13C + 4 * i);
            }
        }
    }
    std::fprintf(file, "%llu %d %x %.3f %.3f %.3f %.3f %.3f %.3f %.3f\n",
                 static_cast<unsigned long long>(conker::current_vi()), static_cast<int>(MEM_W(0, kseg0(kFrameTicks))),
                 static_cast<unsigned>(MEM_W(0, kseg0(kScene))), read_float(rdram, kFrameDelta), position[0],
                 position[1], position[2], eye[0], eye[1], eye[2]);
#if defined(_WIN32)
    std::fflush(file);
#endif
}

}  // namespace

// func_1501C880, after it queues a frame's task on D_8003B1E8.
extern "C" void conker_60fps_task_queued(uint8_t* rdram, recomp_context* /*ctx*/) {
    log_frame(rdram);
    conker::latency::task_queued();
    if (enabled()) {
        wake_scheduler(rdram);
    }
}

// func_80005020, after it swaps a frame in and tells the game the frame is done. The next frame may
// already be queued.
extern "C" void conker_60fps_swapped(uint8_t* rdram, recomp_context* /*ctx*/) {
    if (enabled()) {
        wake_scheduler(rdram);
    }
}

// func_800049E0, after each message: on a wake, try to start the queued frame, as a retrace would
// (it too waits while an audio task runs).
extern "C" void conker_60fps_scheduler_message(uint8_t* rdram, recomp_context* ctx) {
    if (static_cast<int32_t>(MEM_W(kMessageSlot, ctx->r29)) != kWakeMessage) {
        return;
    }
    if (MEM_BU(0, kseg0(kAudioTaskRunning)) == 0) {
        func_80004DB0(rdram, ctx);
    }
}

// Integer steps written as (rate * ticks) >> 1, which lose the odd half: at one retrace a frame, a
// rate of 1 never moves and 3 moves at two thirds of its speed. Called before the shift with the
// product in a register; at one retrace it becomes rate + D_800BE9A0, which alternates 0 and 1, so
// the steps of two frames, rate >> 1 and (rate + 1) >> 1, add up to the rate, as one 30 fps frame
// would. func_15116BAC and func_151169B4 move an object's s16 position toward a target (with a
// sound, so doors and platforms); func_151934B4 drops a falling object's height by its speed;
// func_8000BF60 counts down the title scene's fade of sound 0x22, which never reached 0.
extern "C" void conker_60fps_half_step(uint8_t* rdram, gpr* product) {
    if (enabled() && MEM_W(0, kseg0(kFrameTicks)) == 1) {
        *product = static_cast<gpr>(static_cast<int32_t>(*product) + MEM_BU(0, kseg0(kHalfSteps)));
    }
}

// Steps of (ticks * speed) >> n with n > 1, where splitting the 30 fps step over two frames would
// still round most of it away. Called before the shift with the product in a register; at one
// retrace a frame it becomes 2 * product on the frames that take a 30 Hz step and 0 on the others,
// so the object moves exactly as at 30 fps. func_1508F9F4 drops an entry of the six-slot effect at
// D_800D2460 by (ticks * speed) >> 4 while its speed (+0xC) climbs a tick at a time to 32: below 16
// it never moved at 60 fps.
extern "C" void conker_60fps_full_step(uint8_t* rdram, gpr* product) {
    if (enabled() && MEM_W(0, kseg0(kFrameTicks)) == 1) {
        *product = static_cast<gpr>(static_cast<int32_t>(*product) * 2 * MEM_BU(0, kseg0(kHalfSteps)));
    }
}

// Easing that moves a value a fixed share k of the way to its target every frame settles twice as fast
// at one retrace a frame. Two 60 fps frames with 1 - sqrt(1 - k) each leave the same distance as one
// 30 fps frame with k.
static float per_frame_easing(uint8_t* rdram, float share) {
    if (enabled() && MEM_W(0, kseg0(kFrameTicks)) == 1 && share > 0.0f && share < 1.0f) {
        return 1.0f - std::sqrt(1.0f - share);
    }
    return share;
}

// The Doppler shift of a moving sound (func_80011624, flag 4): (previous distance - distance) *
// ticks / 3 sets the pitch ratio, and the pitch then moves 0.3 of the way to it every frame
// (D_8002C404). Both are tuned for two retraces a frame: at one, a frame's distance is half as far
// and is multiplied by 1 instead of 2, so the shift was a quarter of the 30 fps one, and the pitch
// settled twice as fast. The first is called before the divide with the product in $v0, the second
// before the multiply with the factor in $f10.
extern "C" void conker_60fps_doppler_distance(uint8_t* rdram, recomp_context* ctx) {
    if (enabled() && MEM_W(0, kseg0(kFrameTicks)) == 1) {
        ctx->r2 = static_cast<gpr>(static_cast<int32_t>(ctx->r2) * 4);
    }
}

extern "C" void conker_60fps_doppler_easing(uint8_t* rdram, recomp_context* ctx) {
    ctx->f10.fl = per_frame_easing(rdram, ctx->f10.fl);
}

// func_1501B320 eases two angles per player (D_800BE6C0[p], D_800BE6D0[p]) toward the heading at
// +0x388 of the player's record (*D_800DBFF0, 0x9A0 each) by a fixed share every frame: 0.1 or 0.03
// (D_80096910/D_80096914), or 1 when it should follow at once. Called after the share is loaded into
// $f6 from sp+0x38, where it is read again for the second angle; both get the 60 fps share.
extern "C" void conker_60fps_heading_easing(uint8_t* rdram, recomp_context* ctx) {
    const float share = per_frame_easing(rdram, ctx->f6.fl);
    if (share != ctx->f6.fl) {
        ctx->f6.fl = share;
        MEM_W(0x38, ctx->r29) = static_cast<int32_t>(ctx->f6.u32l);
    }
}

// Counters that go up once a frame where they should go up once a 30 Hz step. Called before the
// increment with the old value in a register; at one retrace a frame it is lowered by one unless
// this frame takes a step, so the counter keeps its 30 fps rate. func_15043384 (the HUD text pass)
// counts D_80085CD0, whose (count >> 1) % 10 picks the frame of the wobbling joystick sprite (kind
// 0x1A, D_80085AA8) beside the selected row of the OPTIONS and pause menus. func_151934B4 adds 1 to
// a falling object's speed (+0x94, up to 20), so it built up speed twice as fast.
extern "C" void conker_60fps_frame_counter(uint8_t* rdram, gpr* count) {
    if (enabled() && MEM_W(0, kseg0(kFrameTicks)) == 1 && MEM_BU(0, kseg0(kHalfSteps)) == 0) {
        *count = static_cast<gpr>(static_cast<int32_t>(*count) - 1);
    }
}

// Timers that add the frame's ticks and act when they pass a threshold. At 30 fps they move in steps
// of 2, so a threshold that falls between two steps is reached a retrace late. Called with the
// ticks in a register; at one retrace a frame they become 2 * D_800BE9A0 (2 on every second frame),
// so the timer moves in 30 Hz steps and acts on the same retrace it would at 30 fps. func_151DE8F0
// (the title menu's input pass) repeats a held stick direction with two per player, +0x36 for left
// and right and +0x3A for up and down, from 0 to 0x33 and then from 0x28 again: one repeat every 12
// retraces at 30 fps, every 11 at 60 without this.
extern "C" void conker_60fps_step_ticks(uint8_t* rdram, gpr* ticks) {
    if (enabled() && static_cast<int32_t>(*ticks) == 1) {
        *ticks = static_cast<gpr>(2 * MEM_BU(0, kseg0(kHalfSteps)));
    }
}

// Random chances rolled once a frame: `if (rand() % 1000 < 300)` and the like, where rand is
// func_150ADA20. At one retrace a frame they are rolled twice as often, so the event comes up twice
// as often. Called right after the roll with its result in $v0: on a frame that takes no 30 Hz step,
// the result becomes `miss`, a value the site's own test always reads as "not this time", so each
// 30 Hz step rolls once, as at 30 fps. The sites (config/us.toml) were found by counting each call
// site's rolls per retrace at both rates over the regression scenes, and their tests read from the
// MIPS after the call.
extern "C" void conker_60fps_chance(uint8_t* rdram, recomp_context* ctx, uint32_t miss) {
    if (!conker::frame_rate::thirty_hz_step(rdram)) {
        ctx->r2 = static_cast<gpr>(static_cast<int32_t>(miss));
    }
}

// The same for func_150ADA68, the float roll in [0, 1] in $f0 (its 16 random bits times 1/65535),
// at sites testing `roll < p` with p at most 1: 1.0, a roll it can return, never passes.
extern "C" void conker_60fps_chance_float(uint8_t* rdram, recomp_context* ctx) {
    if (!conker::frame_rate::thirty_hz_step(rdram)) {
        ctx->f0.fl = 1.0f;
    }
}

// A countdown in a register, lowered once a frame: it is put back on a frame that takes no 30 Hz
// step, so it counts steps. func_15178268 (an insect of a swarm, scene 6) turns towards its target
// when the low nibble of +0x18 reaches 0, then counts it down from 10 (2 near its target) by one a
// frame, so at 60 fps it changed course twice as often.
extern "C" void conker_60fps_countdown(uint8_t* rdram, gpr* count) {
    if (!conker::frame_rate::thirty_hz_step(rdram)) {
        *count = static_cast<gpr>(static_cast<int32_t>(*count) + 1);
    }
}

// A bit flipped once a frame (an animation's two frames): kept as it was on a frame that takes no
// 30 Hz step. func_15178268 flips bit 8 of the insect's +0x1C, its wings.
extern "C" void conker_60fps_toggle(uint8_t* rdram, gpr* flipped, gpr before) {
    if (!conker::frame_rate::thirty_hz_step(rdram)) {
        *flipped = before;
    }
}

// An emitter that adds its rate to a float accumulator once a frame, without the frame's delta, and
// spawns a burst each time it passes 1: func_1515D130 (the emitters in the D_800A64A0 table, which
// spawn through func_15152B38) adds rate + random * range in $f10. At 60 fps the addition is scaled
// by the frame's delta (D_800BE9A4, 0.5 at one retrace a frame), so it spawns as often as at 30.
extern "C" void conker_60fps_emitter_rate(uint8_t* rdram, float* rate) {
    if (enabled()) {
        *rate *= read_float(rdram, kFrameDelta);
    }
}

// func_80004DB0 starts a frame only if its framebuffer is neither the one the VI shows now
// (osViGetCurrentFramebuffer, $v0 here) nor the one it will show next. There are two framebuffers,
// so a frame that starts in the retrace its predecessor was swapped in draws into the one still on
// screen. RT64 does not scan RDRAM out, so only the "next" check is kept.
extern "C" void conker_60fps_current_framebuffer(uint8_t* /*rdram*/, recomp_context* ctx) {
    if (enabled()) {
        ctx->r2 = 0;
    }
}

bool conker::frame_rate::thirty_hz_step(uint8_t* rdram) {
    return !enabled() || MEM_W(0, kseg0(kFrameTicks)) != 1 || MEM_BU(0, kseg0(kHalfSteps)) != 0;
}

bool conker::frame_rate::sixty_fps() { return enabled(); }

bool conker::frame_rate::smooth() { return settings().smooth.load(std::memory_order_relaxed); }

conker::frame_rate::Choice conker::frame_rate::choice() {
    if (!enabled()) {
        return smooth() ? Choice::ThirtySmooth : Choice::Thirty;
    }
    return smooth() ? Choice::SixtySmooth : Choice::Sixty;
}

void conker::frame_rate::request_choice(Choice choice) {
    settings().sixty.store(choice == Choice::Sixty || choice == Choice::SixtySmooth);
    settings().smooth.store(choice == Choice::SixtySmooth || choice == Choice::ThirtySmooth);
    save_settings();
}

nlohmann::json conker::status::frame_rate() {
    return enhancement("frame_rate", "60 fps", enabled(), "CONKER_60FPS");
}

nlohmann::json conker::status::smooth_motion() {
    return enhancement("smooth_motion", "Smooth motion (frame interpolation)", conker::frame_rate::smooth(),
                       "CONKER_SMOOTH");
}

conker::switches::Switch conker::switches::frame_rate() {
    return {"60 FPS", enabled, [](bool on) { settings().sixty.store(on); }};
}

// Only this machine's pictures: never part of an online host's settings.
conker::switches::Switch conker::switches::smooth_motion() {
    return {"SMOOTH MOTION", conker::frame_rate::smooth, [](bool on) { settings().smooth.store(on); }, true};
}
