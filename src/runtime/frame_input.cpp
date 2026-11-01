// One input record per game frame and the fixed step; see frame_input.hpp. Native helpers called
// from the [[patches.hook]] snippets in config/us.toml.
#include "runtime/frame_input.hpp"

#include <fstream>
#include <algorithm>
#include <filesystem>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

#include "recomp.h"
#include "librecomp/game.hpp"
#include "build_version.h"
#include "enhancements/consistent_controls/mouse_look.hpp"
#include "enhancements/frame_rate/frame_rate.hpp"
#include "enhancements/my_view/my_view.hpp"
#include "enhancements/switches.hpp"
#include "platform/display_mode.hpp"
#include "runtime/game.hpp"
#include "runtime/net_play.hpp"
#include "runtime/state_hash.hpp"
#include "ultramodern/ultramodern.hpp"
#include "runtime/snapshot.hpp"

namespace conker::trace { void game_frame(uint32_t frame); }

namespace {

using conker::frame_input::Mouse;
using conker::frame_input::Port;
using conker::frame_input::Record;

inline gpr kseg0(uint32_t address) { return static_cast<gpr>(static_cast<int32_t>(address)); }

constexpr uint32_t kFrameTicks = 0x800BE9E4;  // D_800BE9E4: retraces this frame steps by

constexpr const char* kCameraSwap = "CAMERA SWAP";  // the header's name for input::camera_swapped

const bool g_fixed_step_variable = [] {
    const char* value = std::getenv("CONKER_FIXED_STEP");
    return value != nullptr && value[0] == '1';
}();

// A net session is known once main has read the command line, before the game starts.
bool fixed_on() { return g_fixed_step_variable || conker::net::active(); }

// The mouse aim speed (the OPTIONS page's MOUSE AIM row, a percentage) scales the mouse's motion
// while it aims (it aimed the frame before, mouse_look::aiming), not while it turns the camera. It
// is applied as the motion goes into the record, so a replay, or another machine running the same
// records, aims as this machine's player did whatever its own setting. It is kept in
// conker-recomp-data/input.cfg ("mouse_aim=<percent>"); CONKER_MOUSE_SENSITIVITY (a scale, 1 =
// 100%) sets it for one run.
std::filesystem::path input_config_path() {
    return std::filesystem::current_path() / "conker-recomp-data" / "input.cfg";
}

int clamp_mouse_speed(int percent) {
    return std::clamp(percent, conker::frame_input::kMinMouseSpeed, conker::frame_input::kMaxMouseSpeed);
}

constexpr int kDefaultMouseAim = 200;  // percent, when input.cfg has none

std::atomic<int> g_mouse_speed{[] {
    if (const char* value = std::getenv("CONKER_MOUSE_SENSITIVITY"); value != nullptr && value[0] != '\0') {
        const float scale = std::strtof(value, nullptr);
        if (scale > 0.0f) {
            return clamp_mouse_speed(static_cast<int>(std::lround(scale * 100.0f)));
        }
    }
    std::ifstream file(input_config_path());
    for (std::string line; std::getline(file, line);) {
        const size_t equals = line.find('=');
        if (equals != std::string::npos && line.substr(0, equals) == "mouse_aim") {
            return clamp_mouse_speed(std::atoi(line.c_str() + equals + 1));
        }
    }
    return kDefaultMouseAim;
}()};

// The live mouse's motion since it was last taken, scaled by the aim speed while the port's mouse
// aims.
void take_live_mouse(int port, float* dx, float* dy, int* zoom) {
    conker::input::take_mouse(dx, dy, zoom);
    if (!conker::mouse_look::aiming(port)) {
        return;
    }
    const float scale = static_cast<float>(g_mouse_speed.load(std::memory_order_relaxed)) / 100.0f;
    *dx *= scale;
    *dy *= scale;
}

std::mutex g_mutex;
Record g_record;          // under g_mutex
bool g_started = false;   // under g_mutex: a frame has been sampled
bool g_mouse_taken[conker::input::kPorts] = {};  // under g_mutex: a port's mouse motion this frame handed out
thread_local bool t_game_thread = false;  // set on the thread that runs the game frames
uint32_t g_count_calls = 0;              // game thread only: osGetCount calls this frame
// Native state the game's memory depends on, carried by a snapshot (runtime/snapshot.hpp): the last
// frame's record (its frame number is the game's count and clock).
const bool g_snapshot_fields = [] {
    // The mice's motion goes into float maths and their flag is a bool.
    conker::snapshot::add("frame_input.record", &g_record, sizeof(g_record), [](const uint8_t* bytes) {
        for (int i = 0; i < conker::input::kPorts; i++) {
            const size_t at = offsetof(Record, mice) + i * sizeof(Mouse);
            float dx, dy;
            std::memcpy(&dx, bytes + at + offsetof(Mouse, dx), sizeof(dx));
            std::memcpy(&dy, bytes + at + offsetof(Mouse, dy), sizeof(dy));
            if (!std::isfinite(dx) || !std::isfinite(dy) || bytes[at + offsetof(Mouse, captured)] > 1) {
                return false;
            }
        }
        return true;
    });
    conker::snapshot::add_bools("frame_input.started", &g_started, 1);
    conker::snapshot::add_bools("frame_input.mouse_taken", g_mouse_taken, conker::input::kPorts);
    return true;
}();

// librecomp's osGetCount asks this first (tools/patches/n64modernruntime-count-override.patch).
// The game uses the CPU count as randomness: func_15008340 and others multiply the RNG by it to pick
// points in a region, and on the game thread that count then depends on how fast the frame ran.
// With the fixed step the game thread gets a count made of the frame number and the calls so far
// in the frame instead: 1,562,500 a frame (46.875 MHz over 30), 4,096 a call. Other threads (Rare's
// VI manager keeps the OS time with it) keep the clock.
extern "C" bool (*recomp_get_count_override)(uint32_t* count);

bool frame_count(uint32_t* count) {
    if (!t_game_thread) {
        return false;
    }
    uint32_t frame;
    {
        std::lock_guard lock(g_mutex);
        frame = g_record.frame;
    }
    *count = frame * 1562500u + g_count_calls++ * 4096u;
    return true;
}


// Graphics tasks the game has sent its scheduler (func_1501C880) and display lists RT64 has
// finished, after which the frame's colour and depth are back in RDRAM.
std::mutex g_gfx_mutex;
uint64_t g_gfx_sent = 0;          // game thread only
uint64_t g_gfx_done = 0;          // under g_gfx_mutex
std::atomic<bool> g_gfx_queue_made{false};

// The queue the renderer tells the game thread on when it finishes a task: an OSMesgQueue with
// its slots in the scratch RDRAM above the game's 4 MiB (see ultra_extras.cpp). The game's threads
// take turns (one runs at a time, swapping only in OS calls), so the game thread waits in
// osRecvMesg, which lets Rare's scheduler thread run and start the task being waited for.
constexpr uint32_t kGfxDoneQueue = 0x807FC000;
constexpr uint32_t kGfxDoneSlots = 0x807FC020;
constexpr int32_t kGfxDoneSlotCount = 8;

uint64_t gfx_done() {
    std::lock_guard lock(g_gfx_mutex);
    return g_gfx_done;
}

// With the fixed step: wait for RT64 to finish every graphics task the game has sent. The game reads
// the depth buffer in RDRAM (see the callers), and what it holds depends on whether RT64 has got
// through the last display list yet.
void wait_for_gfx(uint8_t* rdram) {
    // The renderer tells on every task, waited for or not: take what is there first.
    while (osRecvMesg(rdram, static_cast<int32_t>(kGfxDoneQueue), NULLPTR, OS_MESG_NOBLOCK) == 0) {
    }
    while (gfx_done() < g_gfx_sent) {
        osRecvMesg(rdram, static_cast<int32_t>(kGfxDoneQueue), NULLPTR, OS_MESG_BLOCK);
    }
    // More finished than sent would mean tasks sent some other way, and the wait no longer waits for
    // the right one.
    static bool warned = false;
    if (gfx_done() > g_gfx_sent && !warned) {
        warned = true;
        std::fprintf(stderr, "[conker] fixed step: RT64 finished %llu graphics tasks but the game sent %llu\n",
                     static_cast<unsigned long long>(gfx_done()), static_cast<unsigned long long>(g_gfx_sent));
    }
}

int32_t fixed_ticks() { return conker::frame_rate::sixty_fps() ? 1 : 2; }

int8_t stick_axis(float value) { return static_cast<int8_t>(value * 80.0f); }  // as the PIF answers

int8_t right_axis(float value) {
    const float scaled = value * 127.0f;
    return static_cast<int8_t>(scaled < -127.0f ? -127.0f : scaled > 127.0f ? 127.0f : scaled);
}

Port sample_port(int index) {
    Port port;
    if (!conker::input::connected(index)) {
        return port;
    }
    float x = 0.0f;
    float y = 0.0f;
    conker::input::read(&port.buttons, &x, &y, index);
    if (index == 0) {
        port.buttons |= conker::scripted_buttons();
    }
    port.stick_x = stick_axis(x);
    port.stick_y = stick_axis(y);
    port.flags = Port::kConnected;
    float right_x = 0.0f;
    float right_y = 0.0f;
    if (conker::input::right_stick(&right_x, &right_y, index)) {
        port.flags |= Port::kRightStick;
        port.right_x = right_axis(right_x);
        port.right_y = right_axis(right_y);
    }
    if (conker::input::has_rumble(index)) {
        port.flags |= Port::kRumble;
    }
    return port;
}

Record sample_live() {
    Record record;
    for (int i = 0; i < conker::input::kPorts; i++) {
        record.ports[i] = sample_port(i);
    }
    record.mice[0].captured = conker::input::mouse_captured();
    return record;
}

// The record before the first frame: the live input, or online the session's ports.
Record boot_record() { return conker::net::active() ? conker::net::boot_record() : sample_live(); }

// A "# switch LABEL=0|1" line of a replay (or the host's settings): sets that switch as the
// recording (or the host) has it.
void apply_header_line(const char* line, const char* source, bool from_another_machine) {
    char label[64];
    int on = 0;
    if (std::sscanf(line, "# switch %63[^=]=%d", label, &on) == 2) {
        if (std::strcmp(label, kCameraSwap) == 0) {
            if (conker::input::camera_swapped() != (on != 0)) {
                conker::input::set_camera_swapped(on != 0);
                std::printf("[conker] %s: %s %s\n", source, label, on ? "on" : "off");
            }
            return;
        }
        for (const conker::switches::Switch& item : conker::switches::all()) {
            if (std::strcmp(item.label, label) == 0) {
                if (item.local && from_another_machine) {
                    return;  // this machine's own (it writes files): not another machine's to set
                }
                if (item.on() != (on != 0)) {
                    item.set(on != 0);
                    std::printf("[conker] %s: %s %s\n", source, label, on ? "on" : "off");
                }
                return;
            }
        }
        std::printf("[conker] %s: switch %s is not in this build\n", source, label);
        return;
    }
    char build[128];
    int fixed = 0;
    if (std::sscanf(line, "# conker-replay 1 build=%127s fixed_step=%d", build, &fixed) == 2) {
        if (std::strcmp(build, CONKER_BUILD_VERSION) != 0) {
            std::printf("[conker] %s: made with build %s, this is %s; it may not match\n", source, build,
                        CONKER_BUILD_VERSION);
        }
        if ((fixed != 0) != fixed_on()) {
            std::printf("[conker] %s: made with CONKER_FIXED_STEP=%d; it will not match without it\n", source, fixed);
        }
    }
}

void log_record(const Record& record) {
    static FILE* file = [] {
        const char* path = std::getenv("CONKER_FRAME_INPUT_LOG");
        FILE* opened = path != nullptr && path[0] != '\0' ? std::fopen(path, "w") : nullptr;
        if (opened != nullptr) {
            std::setvbuf(opened, nullptr, _IOLBF, 0);  // runs end by being killed
            std::fputs(conker::frame_input::settings_header().c_str(), opened);
        }
        return opened;
    }();
    if (file == nullptr) {
        return;
    }
    std::fprintf(file, "%u %llu", record.frame, static_cast<unsigned long long>(conker::current_vi()));
    for (const Port& port : record.ports) {
        std::fprintf(file, " %x:%04x,%d,%d,%d,%d", port.flags, port.buttons, port.stick_x, port.stick_y, port.right_x,
                     port.right_y);
    }
    for (const Mouse& mouse : record.mice) {
        std::fprintf(file, " m%d:%.9g,%.9g,%d", mouse.captured, mouse.dx, mouse.dy, mouse.zoom);
    }
    std::fputc('\n', file);
}

// CONKER_FRAME_INPUT_REPLAY=<file>: the records of a CONKER_FRAME_INPUT_LOG, by frame number.
const std::vector<Record>& replay() {
    static const std::vector<Record> records = [] {
        std::vector<Record> loaded;
        const char* path = std::getenv("CONKER_FRAME_INPUT_REPLAY");
        FILE* file = path != nullptr && path[0] != '\0' ? std::fopen(path, "r") : nullptr;
        if (file == nullptr) {
            if (path != nullptr && path[0] != '\0') {
                std::fprintf(stderr, "[conker] CONKER_FRAME_INPUT_REPLAY=%s: cannot open it\n", path);
            }
            return loaded;
        }
        char line[512];
        while (std::fgets(line, sizeof(line), file) != nullptr) {
            if (line[0] == '#') {
                apply_header_line(line, "replay", false);
                continue;
            }
            Record record;
            unsigned long long vi = 0;
            int at = 0;
            if (std::sscanf(line, "%u %llu%n", &record.frame, &vi, &at) != 2 || record.frame != loaded.size() + 1) {
                break;
            }
            bool ok = true;
            for (Port& port : record.ports) {
                unsigned flags = 0, buttons = 0;
                int x = 0, y = 0, right_x = 0, right_y = 0, used = 0;
                ok = ok && std::sscanf(line + at, " %x:%x,%d,%d,%d,%d%n", &flags, &buttons, &x, &y, &right_x, &right_y,
                                       &used) == 6;
                at += used;
                port = {static_cast<uint16_t>(buttons), static_cast<int8_t>(x), static_cast<int8_t>(y),
                        static_cast<int8_t>(right_x), static_cast<int8_t>(right_y), static_cast<uint8_t>(flags)};
            }
            // A mouse a port; logs from before M3 have controller 1's alone.
            for (int i = 0; ok && i < conker::input::kPorts; i++) {
                Mouse& mouse = record.mice[i];
                int captured = 0, zoom = 0, used = 0;
                if (std::sscanf(line + at, " m%d:%f,%f,%d%n", &captured, &mouse.dx, &mouse.dy, &zoom, &used) != 4) {
                    ok = i > 0;
                    mouse = {};
                    break;
                }
                at += used;
                mouse.captured = captured != 0;
                mouse.zoom = static_cast<int8_t>(zoom);
            }
            if (!ok) {
                break;
            }
            loaded.push_back(record);
        }
        std::fclose(file);
        std::printf("[conker] replaying %zu frames of input from %s\n", loaded.size(), path);
        return loaded;
    }();
    return records;
}

}  // namespace

namespace conker::frame_input {

bool fixed_step() { return fixed_on(); }

uint32_t count_reads() { return g_count_calls; }
void set_count_reads(uint32_t reads) { g_count_calls = reads; }

std::string settings_header() {
    std::string text = "# conker-replay 1 build=" + std::string(CONKER_BUILD_VERSION) +
                       " fixed_step=" + (fixed_on() ? "1" : "0") + "\n";
    for (const switches::Switch& item : switches::all()) {
        text += "# switch " + std::string(item.label) + "=" + (item.on() ? "1" : "0") + "\n";
    }
    text += "# switch " + std::string(kCameraSwap) + "=" + (input::camera_swapped() ? "1" : "0") + "\n";
    return text;
}

void apply_settings(const std::string& header, const char* source) {
    size_t at = 0;
    while (at < header.size()) {
        size_t end = header.find('\n', at);
        if (end == std::string::npos) {
            end = header.size();
        }
        apply_header_line(header.substr(at, end - at).c_str(), source, true);
        at = end + 1;
    }
}

std::chrono::steady_clock::time_point clock_now() {
    if (!fixed_on()) {
        return std::chrono::steady_clock::now();
    }
    uint32_t frame;
    {
        std::lock_guard lock(g_mutex);
        frame = g_record.frame;
    }
    const int64_t rate = frame_rate::sixty_fps() ? 60 : 30;
    return std::chrono::steady_clock::time_point(std::chrono::microseconds(int64_t{frame} * 1000000 / rate));
}

void gfx_task_done() {
    {
        std::lock_guard lock(g_gfx_mutex);
        g_gfx_done++;
    }
    if (g_gfx_queue_made.load()) {
        ultramodern::enqueue_external_message(static_cast<int32_t>(kGfxDoneQueue), 0, false, true);
    }
}

Record current() {
    {
        std::lock_guard lock(g_mutex);
        if (g_started) {
            return g_record;
        }
    }
    return boot_record();
}

Port port(int index) {
    if (index < 0 || index >= input::kPorts) {
        return {};
    }
    {
        std::lock_guard lock(g_mutex);
        if (g_started) {
            return g_record.ports[index];
        }
    }
    return net::active() ? net::boot_record().ports[index] : sample_port(index);
}

int mouse_speed() { return g_mouse_speed.load(); }

void set_mouse_speed(int percent) {
    percent = clamp_mouse_speed(percent);
    if (g_mouse_speed.exchange(percent) == percent) {
        return;
    }
    std::printf("[conker] mouse aim %d%%\n", percent);
    std::ofstream file(input_config_path());
    file << "mouse_aim=" << percent << '\n';
    if (!file) {
        std::fprintf(stderr, "[conker] could not write %s\n", input_config_path().string().c_str());
    }
}

void read(uint16_t* buttons, float* x, float* y, int index) {
    const Port p = port(index);
    *buttons = p.buttons;
    *x = p.stick_x / 80.0f;
    *y = p.stick_y / 80.0f;
}

bool right_stick(float* x, float* y, int index) {
    const Port p = port(index);
    const bool has = (p.flags & Port::kRightStick) != 0;
    *x = has ? p.right_x / 127.0f : 0.0f;
    *y = has ? p.right_y / 127.0f : 0.0f;
    return has;
}

bool connected(int index) { return (port(index).flags & Port::kConnected) != 0; }

bool has_rumble(int index) { return (port(index).flags & Port::kRumble) != 0; }

bool mouse_captured(int port) {
    if (port < 0 || port >= input::kPorts) {
        return false;
    }
    {
        std::lock_guard lock(g_mutex);
        if (g_started) {
            return g_record.mice[port].captured;
        }
    }
    return port == 0 && !net::active() && input::mouse_captured();
}

void take_mouse(int port, float* dx, float* dy, int* zoom) {
    *dx = 0.0f;
    *dy = 0.0f;
    *zoom = 0;
    if (port < 0 || port >= input::kPorts) {
        return;
    }
    {
        std::lock_guard lock(g_mutex);
        if (g_started) {
            if (!g_mouse_taken[port]) {
                g_mouse_taken[port] = true;
                *dx = g_record.mice[port].dx;
                *dy = g_record.mice[port].dy;
                *zoom = g_record.mice[port].zoom;
            }
            return;
        }
    }
    if (port == 0 && !net::active()) {
        take_live_mouse(0, dx, dy, zoom);
    }
}

}  // namespace conker::frame_input

// CONKER_SNAPSHOT_SAVE=<frame>:<file> writes a snapshot of the game at the start of that frame;
// CONKER_SNAPSHOT_LOAD=<file> puts one in place at the first frame it can (from frame 3), after
// which the run goes on from the snapshot's frame (with CONKER_FRAME_INPUT_REPLAY, the same replay).
void test_snapshot(uint8_t* rdram, recomp_context* ctx) {
    static const char* save_spec = std::getenv("CONKER_SNAPSHOT_SAVE");
    static const char* load_path = std::getenv("CONKER_SNAPSHOT_LOAD");
    static bool loaded = false;
    uint32_t next;
    {
        std::lock_guard lock(g_mutex);
        next = g_record.frame + 1;
    }
    if (save_spec != nullptr) {
        char* end;
        const uint32_t at = static_cast<uint32_t>(std::strtoul(save_spec, &end, 0));
        if (next == at && *end == ':') {
            std::string why;
            const std::vector<uint8_t> data = conker::snapshot::save(rdram, ctx, next, &why);
            std::ofstream(end + 1, std::ios::binary).write(reinterpret_cast<const char*>(data.data()),
                                                             static_cast<std::streamsize>(data.size()));
            std::printf("[conker] snapshot of frame %u: %zu bytes in %s\n", next, data.size(), end + 1);
        }
    }
    if (load_path != nullptr && !loaded && next >= 3) {
        loaded = true;
        std::ifstream file(load_path, std::ios::binary);
        const std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        std::string why;
        if (conker::snapshot::load(rdram, ctx, data, &why)) {
            std::printf("[conker] snapshot of frame %u put in place at frame %u\n", conker::snapshot::frame_of(data), next);
        } else {
            std::printf("[conker] snapshot not loaded at frame %u: %s\n", next, why.c_str());
        }
    }
}

// CONKER_SNAPSHOT_SELFTEST=<frame>: snapshot::self_test at that frame (tampered snapshots refused).
// CONKER_RUNTIME_SELFTEST=<frame>:queue|thread: at that frame, a message queue in scratch RDRAM is given
// out-of-range indices, or a thread record of this run's is copied elsewhere and put on it, and a message
// is sent: the runtime must stop the game rather than write through them.
// CONKER_RUNTIME_SELFTEST=<frame>:guard_high|guard_low: a store just past either end of the 4 GiB, which
// the guard pages must stop.
// CONKER_RUNTIME_SELFTEST=<frame>:pi|pi_size: a ROM read outside the cartridge: the runtime must stop the
// game.
// CONKER_RUNTIME_SELFTEST=<frame>:ai: an audio buffer of a gigabyte: the runtime must stop the game.
void test_runtime_checks(uint8_t* rdram, recomp_context* ctx, uint32_t frame) {
    static const char* snapshot_spec = std::getenv("CONKER_SNAPSHOT_SELFTEST");
    if (snapshot_spec != nullptr && std::strtoul(snapshot_spec, nullptr, 0) == frame) {
        for (const std::string& line : conker::snapshot::self_test(rdram, ctx, frame)) {
            std::printf("[conker] snapshot self-test: %s\n", line.c_str());
        }
        std::fflush(stdout);
    }
    static const char* runtime_spec = std::getenv("CONKER_RUNTIME_SELFTEST");
    if (runtime_spec == nullptr) {
        return;
    }
    char* end;
    if (std::strtoul(runtime_spec, &end, 0) != frame || *end != ':') {
        return;
    }
    const std::string kind = end + 1;
    if (kind == "guard_high" || kind == "guard_low") {
        // A store the way recompiled code makes one, through a pointer the host could have left in
        // game memory: 0x7FFFFFF0 plus 0x10, or 0x80000000 minus 0x8000, lands just outside the 4 GiB
        //. The guard must stop it: the crash handler names it and the process exits.
        const bool high = kind == "guard_high";
        const gpr pointer = static_cast<gpr>(static_cast<int64_t>(static_cast<int32_t>(high ? 0x7FFFFFF0u : 0x80000000u)));
        std::printf("[conker] runtime self-test (%s): storing through 0x%08X%+d; the guard must stop it\n", kind.c_str(),
                    static_cast<uint32_t>(pointer), high ? 0x10 : -0x8000);
        std::fflush(stdout);
        if (high) {
            *reinterpret_cast<volatile int32_t*>(&MEM_W(0x10, pointer)) = 0x51F7E57;
        } else {
            *reinterpret_cast<volatile int32_t*>(&MEM_W(-0x8000, pointer)) = 0x51F7E57;
        }
        std::printf("[conker] runtime self-test (%s): FAIL, the store went through\n", kind.c_str());
        std::fflush(stdout);
        return;
    }
    if (kind == "pi" || kind == "pi_size") {
        // A ROM read past the end of the cartridge, or one from inside it that is too long:
        // the runtime must stop before copying anything.
        std::printf("[conker] runtime self-test (%s): the runtime must stop the game now\n", kind.c_str());
        std::fflush(stdout);
        const gpr scratch = static_cast<gpr>(static_cast<int64_t>(static_cast<int32_t>(0x80600000u)));
        const uint32_t rom_end = 0x10000000u + static_cast<uint32_t>(recomp::get_rom().size());
        recomp::do_rom_read(rdram, scratch, kind == "pi" ? rom_end + 0x100000u : rom_end - 0x100u, kind == "pi" ? 0x100 : 0x10000);
        std::printf("[conker] runtime self-test (%s): FAIL, the game went on\n", kind.c_str());
        std::fflush(stdout);
        return;
    }
    if (kind == "ai") {
        // An audio buffer of a gigabyte: the runtime must stop before copying or queueing it.
        std::printf("[conker] runtime self-test (ai): the runtime must stop the game now\n");
        std::fflush(stdout);
        ultramodern::queue_audio_buffer(rdram, static_cast<int32_t>(0x80600000u), 0x40000000u);
        std::printf("[conker] runtime self-test (ai): FAIL, the game went on\n");
        std::fflush(stdout);
        return;
    }
    constexpr uint32_t kQueue = 0x807FC800;   // free scratch RDRAM above the game's 4 MiB
    constexpr uint32_t kBuffer = 0x807FC840;
    constexpr uint32_t kStray = 0x807FC900;
    const int32_t queue = static_cast<int32_t>(kQueue);
    osCreateMesgQueue(rdram, queue, static_cast<int32_t>(kBuffer), 4);
    OSMesgQueue* mq = TO_PTR(OSMesgQueue, queue);
    const std::string what = end + 1;
    if (what == "queue") {
        mq->msgCount = 0x7FFFFFFF;
        mq->first = 0x40000000;
    } else if (what == "thread") {
        OSThread* stray = TO_PTR(OSThread, static_cast<int32_t>(kStray));
        std::memcpy(stray, TO_PTR(OSThread, ultramodern::this_thread()), sizeof(OSThread));
        stray->next = NULLPTR;
        stray->priority = 0x7FFFFFFF;
        stray->state = 3;  // BLOCKED
        mq->blocked_on_recv = static_cast<int32_t>(kStray);
    } else {
        return;
    }
    std::printf("[conker] runtime self-test (%s): sending a message; the runtime must stop the game now\n", what.c_str());
    std::fflush(stdout);
    osSendMesg(rdram, queue, 0, 0);
    std::printf("[conker] runtime self-test (%s): FAIL, the game went on\n", what.c_str());
    std::fflush(stdout);
}

// At the start of func_1501905C, once a game frame before func_1501BBB8 reads the pads: the frame's
// record.
extern "C" void conker_frame_input_begin(uint8_t* rdram, recomp_context* ctx) {
    if (!t_game_thread) {
        t_game_thread = true;
        if (fixed_on()) {
            recomp_get_count_override = frame_count;
        }
        // Online, the host's settings, applied where a replay applies its own.
        if (conker::net::active() && conker::net::local_port() != 0) {
            conker::display::keep_saved_picture();  // the host's WIDESCREEN and SHARP are for the session
            conker::frame_input::apply_settings(conker::net::host_settings(), "net: the host's setting");
        }
    }
    g_count_calls = 0;
    // The frame before's depth: func_1512B1B8, for one, scans it around the camera during the logic.
    if (fixed_on()) {
        wait_for_gfx(rdram);
    }
    conker::my_view::frame(rdram, ctx);  // before the audio frame, which plays what it posts
    conker::frame_input::make_audio_frame(rdram, ctx);
    test_snapshot(rdram, ctx);
    {
        uint32_t next;
        {
            std::lock_guard lock(g_mutex);
            next = g_record.frame + 1;
        }
        test_runtime_checks(rdram, ctx, next);
    }
    Record record = sample_live();
    int zoom = 0;
    take_live_mouse(conker::net::active() ? conker::net::local_port() : 0, &record.mice[0].dx, &record.mice[0].dy, &zoom);
    record.mice[0].zoom = static_cast<int8_t>(zoom < -127 ? -127 : zoom > 127 ? 127 : zoom);
    uint32_t frame;
    {
        std::lock_guard lock(g_mutex);
        frame = g_record.frame + 1;
    }
    // Online: this machine's input goes out for a later frame, and every port's for this one comes in.
    if (conker::net::active()) {
        record.frame = frame;
        conker::net::frame(rdram, &record);
        frame = record.frame;  // a rejoining guest's game may have gone on to the host's snapshot
    }
    static const std::vector<Record> kNoReplay;
    const std::vector<Record>& replayed = conker::net::active() ? kNoReplay : replay();
    {
        std::lock_guard lock(g_mutex);
        if (!replayed.empty()) {
            // Past the end of the replay, every port stays connected as it was, with nothing pressed.
            record = frame <= replayed.size() ? replayed[frame - 1] : Record{};
            if (frame > replayed.size()) {
                for (int i = 0; i < conker::input::kPorts; i++) {
                    record.ports[i].flags = replayed.back().ports[i].flags;
                }
            }
        }
        record.frame = frame;
        g_record = record;
        g_started = true;
        std::fill(std::begin(g_mouse_taken), std::end(g_mouse_taken), false);
    }
    log_record(record);
    conker::state_hash::frame(rdram, record.frame);
    conker::trace::game_frame(record.frame);
}

// At the start of func_15019F20, the frame's time step, which reads the retraces the frame took
// from D_800BE9E4: with CONKER_FIXED_STEP=1 every frame took the same number.
extern "C" void conker_fixed_step(uint8_t* rdram, recomp_context* /*ctx*/) {
    if (fixed_on()) {
        MEM_W(0, kseg0(kFrameTicks)) = fixed_ticks();
    }
}

// In func_80004F00 on the scheduler thread, before it stores the retraces the last frame took ($v0)
// to D_800BE9E4. It does so when it starts a frame's graphics task, which can be while the game
// thread is still in the frame after: the menu ticks in func_151E51EC, for one, read it later in the
// frame than func_15019F20. With the fixed step it stores the same number.
extern "C" void conker_fixed_step_scheduler(uint8_t* /*rdram*/, recomp_context* ctx) {
    if (fixed_on()) {
        ctx->r2 = fixed_ticks();
    }
}

// At the start of func_1501C880, which sends the scheduler a graphics task, once a call.
extern "C" void conker_gfx_task_sent(uint8_t* rdram, recomp_context* /*ctx*/) {
    if (fixed_on() && !g_gfx_queue_made.load()) {
        osCreateMesgQueue(rdram, static_cast<int32_t>(kGfxDoneQueue), static_cast<int32_t>(kGfxDoneSlots),
                          kGfxDoneSlotCount);
        g_gfx_queue_made.store(true);
    }
    g_gfx_sent++;
}

// In func_1501878C, where it handles a graphics task's "done" message, after the frame that
// followed the task. There func_1517E1AC keeps the depth buffer's value under each point on the
// list at D_800DDD64 (+0x2A) and func_151412BC the value under each actor's points in each view
// (+0x15C on). By then the game has sent the next frame's task, and RT64 had written that frame's
// depth into RDRAM in some runs and not in others. With the fixed step the game waits for RT64 to
// finish every task sent, so these read the newest frame's depth in every run.
extern "C" void conker_gfx_done_message(uint8_t* rdram, recomp_context* /*ctx*/) {
    if (fixed_on()) {
        wait_for_gfx(rdram);
    }
}

// In __conker_eeprom_wait (func_151DCFD8), Rare's paced EEPROM write, before it asks
// __osEepromTimerQ whether the 12 ms timer it armed after the last 8-byte block has gone off
// ($a2: block or not). Once a frame the game asks without blocking, so a save took a block a frame
// except when two frames came less than 12 ms apart, as after a stall: then the write ended a
// frame later in one run than in another. With the fixed step it waits for the timer (at most
// 12 ms, as it was armed a frame ago), so every frame writes a block.
extern "C" void conker_eeprom_timer_wait(uint8_t* /*rdram*/, recomp_context* ctx) {
    if (fixed_on()) {
        ctx->r6 = 1;  // OS_MESG_BLOCK
    }
}
