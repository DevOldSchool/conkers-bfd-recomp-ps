// Exact-position saves. Native helpers called from the [[patches.hook]] snippets in config/us.toml
// (func_15006BEC, func_151E327C, func_1501C57C).
//
// The game's save holds progress and the checkpoint to resume from (scene D_800BE3DF, entrance
// D_800BE3E0), not where Conker stands. A save made from the pause menu (SAVE, or QUIT then Y)
// also records the scene, entrance, position and facing in a file next to the save,
// conker_us.resume, tagged with the save's sequence number. Starting that save (PLAY on the title
// menu, or LOAD in the pause menu) then enters the recorded scene by that entrance, and once the
// scene is running, Conker is moved to the recorded spot for the first few frames. (The spawn
// override the scene triggers use, func_15022190, is only read by func_15022BA4, which places
// Conker in the intro and title scenes, not in the game's own.) A checkpoint save made later has
// a newer sequence number, so the game then resumes from that checkpoint as usual, except for the
// save the game makes on entering the area just after resuming, which keeps the point. The
// cartridge save format is unchanged.
#include "resume_point.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "recomp.h"
#include "ultramodern/ultramodern.hpp"

extern "C" void func_151E327C(uint8_t* rdram, recomp_context* ctx);

namespace conker::resume {

namespace {

inline gpr kseg0(uint32_t address) { return static_cast<gpr>(static_cast<int32_t>(address)); }

constexpr uint32_t kPlayerObject = 0x80088720;   // D_80088720: Conker's object
constexpr int kObjectX = 0x14;                   // position, floats
constexpr int kObjectY = 0x18;
constexpr int kObjectZ = 0x1C;
constexpr int kObjectYaw = 0xB8;                 // facing, radians
constexpr uint32_t kScene = 0x800BE9F0;          // D_800BE9F0: the scene being played
constexpr uint32_t kEntrance = 0x800DD2C0;       // D_800DD2C0: the entrance it was entered by
constexpr int kObjectPreviousX = 0x2C;           // last frame's position
constexpr uint32_t kTransition = 0x800BE615;     // D_800BE615: scene change pending, 0 when none
constexpr int kSettleFrames = 10;                // frames the scene runs before Conker is moved
constexpr int kPlaceFrames = 4;                  // frames he is then held at the resumed spot
constexpr uint32_t kSaveCounters = 0x800BE400;   // D_800BE3F8 + 8, 16 bytes per slot
constexpr uint32_t kSaveSlot = 0x8008FDA8;       // D_8008FDA8: the game slot in play
constexpr uint32_t kEepromAction = 0x80082BB4;   // D_80082BB4: request for func_1501C57C
constexpr uint32_t kEepromSlot = 0x800BE3EC;     // D_800BE3EC: its slot argument
constexpr int8_t kEepromLoad = 1;
constexpr int kSlots = 3;
// A save the game makes this soon after resuming (it saves on entering some areas) is still the
// resumed one: the point carries over to it.
constexpr std::chrono::seconds kResumeSaveWindow{10};

struct Point {
    bool valid = false;
    uint32_t counter = 0;  // the save's sequence number
    int scene = 0;
    int entrance = 0;
    float x = 0, y = 0, z = 0, yaw = 0;  // yaw in degrees
};

Point g_pending;           // remembered, waiting for the save to be written
int g_pending_slot = -1;
bool g_load_requested = false;
Point g_resuming;          // being started, waiting for Conker to be placed
int g_resume_frames = 0;
int g_resumed_slot = -1;   // the slot last started at its point, and when
std::chrono::steady_clock::time_point g_resumed_at;

std::filesystem::path points_path() {
    std::filesystem::path path = ultramodern::get_save_file_path();
    path.replace_extension(".resume");
    return path;
}

// One line per slot: "<slot> <counter> <scene> <entrance> <x> <y> <z> <yaw>".
std::array<Point, kSlots> read_points() {
    std::array<Point, kSlots> points{};
    std::ifstream file(points_path());
    for (std::string line; std::getline(file, line);) {
        std::istringstream in(line);
        int slot;
        Point point;
        if (in >> slot >> point.counter >> point.scene >> point.entrance >> point.x >> point.y >> point.z >> point.yaw &&
            slot >= 0 && slot < kSlots) {
            point.valid = true;
            points[slot] = point;
        }
    }
    return points;
}

void write_points(const std::array<Point, kSlots>& points) {
    const std::filesystem::path path = points_path();
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::trunc);
    for (int slot = 0; slot < kSlots; slot++) {
        const Point& p = points[slot];
        if (p.valid) {
            file << slot << ' ' << p.counter << ' ' << p.scene << ' ' << p.entrance << ' ' << p.x << ' ' << p.y << ' '
                 << p.z << ' ' << p.yaw << '\n';
        }
    }
}

void write_float(uint8_t* rdram, uint32_t address, float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    MEM_W(0, kseg0(address)) = static_cast<int32_t>(bits);
}

float read_float(uint8_t* rdram, uint32_t address) {
    const uint32_t bits = static_cast<uint32_t>(MEM_W(0, kseg0(address)));
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

uint32_t save_counter(uint8_t* rdram, int slot) {
    return static_cast<uint32_t>(MEM_W(0, kseg0(kSaveCounters + static_cast<uint32_t>(slot) * 16)));
}

}  // namespace

void remember_position(uint8_t* rdram, int slot) {
    const uint32_t object = static_cast<uint32_t>(MEM_W(0, kseg0(kPlayerObject)));
    if (slot < 0 || slot >= kSlots || (object & 0xFF000000) != 0x80000000) {
        return;
    }
    g_pending.valid = true;
    g_pending.scene = MEM_W(0, kseg0(kScene));
    g_pending.entrance = MEM_BU(0, kseg0(kEntrance));
    g_pending.x = read_float(rdram, object + kObjectX);
    g_pending.y = read_float(rdram, object + kObjectY);
    g_pending.z = read_float(rdram, object + kObjectZ);
    g_pending.yaw = read_float(rdram, object + kObjectYaw) * (180.0f / 3.14159265f);
    g_pending_slot = slot;
}

void request_load(uint8_t* rdram, int slot) {
    if (slot < 0 || slot >= kSlots) {
        return;
    }
    MEM_B(0, kseg0(kEepromAction)) = kEepromLoad;
    MEM_B(0, kseg0(kEepromSlot)) = static_cast<int8_t>(slot);
    g_load_requested = true;
}

}  // namespace conker::resume

using namespace conker::resume;

// In func_15006BEC (write a save) once the slot's new sequence number is stored ($s1 slot,
// $s0 sequence number): if this save came from the pause menu, record where Conker was.
extern "C" void conker_resume_saved(uint8_t* /*rdram*/, recomp_context* ctx) {
    const int slot = static_cast<int32_t>(ctx->r17);
    const uint32_t counter = static_cast<uint32_t>(ctx->r16);
    if (!g_pending.valid || slot != g_pending_slot) {
        if (slot == g_resumed_slot && std::chrono::steady_clock::now() - g_resumed_at < kResumeSaveWindow) {
            std::array<Point, kSlots> points = read_points();
            if (points[slot].valid) {
                points[slot].counter = counter;
                write_points(points);
            }
        }
        return;
    }
    std::array<Point, kSlots> points = read_points();
    points[slot] = g_pending;
    points[slot].counter = counter;
    write_points(points);
    std::printf("[conker] resume point: slot %d, save %u, scene %d entrance %d at (%.0f, %.0f, %.0f) facing %.0f\n",
                slot, points[slot].counter, g_pending.scene, g_pending.entrance, g_pending.x, g_pending.y, g_pending.z,
                g_pending.yaw);
    g_pending.valid = false;
}

// In func_151E327C (start the loaded game, PLAY on the title menu) before
// `jal func_1501C730(6, scene, entrance, ...)`: start at the recorded point if it belongs to this
// very save.
extern "C" void conker_resume_start(uint8_t* rdram, recomp_context* ctx) {
    const int slot = MEM_B(0, kseg0(kSaveSlot));
    if (slot < 0 || slot >= kSlots) {
        return;
    }
    const Point point = read_points()[slot];
    if (!point.valid || point.counter != save_counter(rdram, slot)) {
        return;
    }
    ctx->r5 = point.scene;
    ctx->r6 = point.entrance;
    g_resuming = point;
    g_resume_frames = 0;
    g_resumed_slot = slot;
    g_resumed_at = std::chrono::steady_clock::now();
    std::printf("[conker] resuming slot %d in scene %d at (%.0f, %.0f, %.0f)\n", slot, point.scene, point.x, point.y,
                point.z);
}

// At the start of func_1501C57C, the EEPROM manager, once a frame: when a LOAD's slot read has
// been done, start the game as PLAY does.
// Also moves Conker to the point being resumed once its scene is running.
extern "C" void conker_resume_tick(uint8_t* rdram, recomp_context* ctx) {
    if (g_load_requested && MEM_B(0, kseg0(kEepromAction)) == 0) {
        g_load_requested = false;
        recomp_context saved = *ctx;
        func_151E327C(rdram, ctx);
        *ctx = saved;
        return;
    }
    if (!g_resuming.valid) {
        return;
    }
    const uint32_t object = static_cast<uint32_t>(MEM_W(0, kseg0(kPlayerObject)));
    if (MEM_W(0, kseg0(kScene)) != g_resuming.scene || MEM_BU(0, kseg0(kTransition)) != 0 ||
        (object & 0xFF000000) != 0x80000000) {
        return;
    }
    if (++g_resume_frames <= kSettleFrames) {
        return;
    }
    const float position[3] = {g_resuming.x, g_resuming.y, g_resuming.z};
    for (int i = 0; i < 3; i++) {
        write_float(rdram, object + kObjectX + 4 * i, position[i]);
        write_float(rdram, object + kObjectPreviousX + 4 * i, position[i]);
    }
    write_float(rdram, object + kObjectYaw, g_resuming.yaw * (3.14159265f / 180.0f));
    if (g_resume_frames >= kSettleFrames + kPlaceFrames) {
        g_resuming.valid = false;
    }
}
