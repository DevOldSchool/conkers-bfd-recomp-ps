// Exact-position saves. Native helpers called from the [[patches.hook]] snippets in config/us.toml
// (func_15006BEC, func_151E327C, func_1501C57C).
//
// The game's save holds progress and the checkpoint to resume from (scene D_800BE3DF, entrance
// D_800BE3E0), not where Conker stands. A save made from the pause menu (SAVE, or QUIT then Y)
// also records the scene, entrance, position, Conker's facing and the camera's yaw in a file next
// to the save, conker_us.resume, tagged with the save's sequence number. Starting that save (PLAY on
// the title menu, or LOAD in the pause menu) then enters the recorded scene by that entrance, and
// once the scene is running, Conker is moved to the recorded spot for the first few frames, facing
// as he did, with the camera put back where it looked (as the debug menu's teleport does). (The spawn
// override the scene triggers use, func_15022190, is only read by func_15022BA4, which places
// Conker in the intro and title scenes, not in the game's own.) A checkpoint save made later has
// a newer sequence number, so the game then resumes from that checkpoint as usual, except for the
// save the game makes on entering the area just after resuming, which keeps the point. The
// cartridge save format is unchanged.
//
// Each of those saves is also kept as a snapshot in saves/snapshots/, named
// scene<scene>-<YYYYMMDD>-<HHMMSS>: a .bin with the whole EEPROM, as conker_us.bin holds it, and a
// .resume with the points, as conker_us.resume holds them (the saved slot's line first), so
// copying both over those two files restores it. The debug menu lists them (list_snapshots) and
// starts one (load_snapshot). Rare writes the EEPROM 8 bytes per 12 ms timer tick (func_151DCEF0, drained by
// func_151DCFD8 at the start of func_1501C57C), so the copy is taken once that write is done.
// CONKER_SAVE_SNAPSHOTS=0 turns the snapshots off.
#include "enhancements/pause_save/resume_point.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <string>

#include "recomp.h"
#include "ultramodern/ultramodern.hpp"

extern "C" void func_151E327C(uint8_t* rdram, recomp_context* ctx);
// librecomp's copy of the save (lib/N64ModernRuntime/librecomp/src/pi.cpp), read from and written
// to RDRAM.
void save_read(uint8_t* rdram, int32_t rdram_address, uint32_t offset, uint32_t count);
void save_write(uint8_t* rdram, int32_t rdram_address, uint32_t offset, uint32_t count);

namespace conker::resume {

namespace {

inline gpr kseg0(uint32_t address) { return static_cast<gpr>(static_cast<int32_t>(address)); }

constexpr uint32_t kPlayerObject = 0x80088720;   // D_80088720: Conker's object
constexpr int kObjectX = 0x14;                   // position, floats
constexpr int kObjectY = 0x18;
constexpr int kObjectZ = 0x1C;
constexpr int kObjectYaw = 0xB8;                 // an angle in radians, restored with the position
constexpr int kObjectFacing = 0x40;              // facing, degrees from +Z towards +X (not wrapped)
constexpr int kObjectFacingAngle = 0x76;         // the facing it is made from, and the one turned
constexpr int kObjectFacingTarget = 0x7A;        // towards: 16-bit angles from +X, 0x10000 a turn
constexpr int kObjectCamera = 0x31C;             // the camera block: forward vector, eye, look-at
constexpr int kCameraForward = 0x130;
constexpr int kCameraEye = 0x13C;
constexpr int kCameraAt = 0x148;
// The camera keeps its own place in the world and is pulled along after Conker; its angles are
// worked out from that place every frame. So a teleport puts the camera itself at the yaw asked for,
// as far from the point it looks at, and at the pitch, as it was (the game keeps the camera at the
// height it wants).
constexpr float kLookAtHeight = 67.5f;           // the camera looks at Conker's position + this
constexpr int kObjectCameraControl = 0x318;      // the camera's controls, with
constexpr int kControlEyes[] = {0x2EC, 0x2F8, 0x304};  // copies of its position,
constexpr int kOrbitDegrees = 0x37C;             // its turn around Conker (yaw + 180, degrees),
constexpr int kOrbitTargetDegrees = 0x380;       // the turn it eases towards,
constexpr int kCameraBehind = 0x390;             // Conker's facing - the camera's yaw,
constexpr int kOrbitRadians = 0x39C;             // and the two turns in radians
constexpr int kOrbitTargetRadians = 0x3A0;
constexpr uint32_t kScene = 0x800BE9F0;          // D_800BE9F0: the scene being played
constexpr uint32_t kEntrance = 0x800DD2C0;       // D_800DD2C0: the entrance it was entered by
constexpr int kObjectPreviousX = 0x2C;           // last frame's position
constexpr uint32_t kTransition = 0x800BE615;     // D_800BE615: scene change pending, 0 when none
constexpr int kSettleFrames = 10;                // frames the scene runs before Conker is moved
constexpr int kPlaceFrames = 4;                  // frames he is then held at the resumed spot
constexpr int kTeleportFrames = 8;               // frames he is held after a teleport (debug menu)
constexpr uint32_t kSaveCounters = 0x800BE400;   // D_800BE3F8 + 8, 16 bytes per slot
constexpr uint32_t kSaveSlot = 0x8008FDA8;       // D_8008FDA8: the game slot in play
constexpr uint32_t kEepromAction = 0x80082BB4;   // D_80082BB4: request for func_1501C57C
constexpr uint32_t kEepromSlot = 0x800BE3EC;     // D_800BE3EC: its slot argument
constexpr int8_t kEepromLoad = 1;
constexpr uint32_t kEepromWriting = 0x800E0A20;  // D_800E0A20: paced EEPROM write under way, 0 when none
constexpr uint32_t kEepromSize = 0x800;          // 16 kbit
constexpr uint32_t kSnapshotScratch = 0x807F9000;  // free RDRAM, see pause_save.cpp
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
    bool has_view = false;               // facing and camera_yaw recorded (see Position)
    float facing = 0, camera_yaw = 0;
};

Point g_pending;           // remembered, waiting for the save to be written
int g_pending_slot = -1;
bool g_load_requested = false;
Point g_resuming;          // being started, waiting for Conker to be placed
int g_resume_frames = 0;
int g_place_frames = kPlaceFrames;
Position g_view;           // a teleport's facing and camera, held with its position
float g_camera_distance = 600.0f;  // from the camera to the point it looks at, and its pitch,
float g_camera_pitch = -30.0f;     // when a teleport was asked for or a resumed save first placed
// A resumed save's camera is put back once it has settled behind Conker (it swings into place as
// the scene starts), then held there for a few frames.
bool g_camera_waits = false;
int g_camera_still = 0;            // frames it has not moved
int g_camera_wait = 0;             // frames waited
int g_camera_hold = 0;             // frames left to hold it
float g_last_eye[3] = {};
constexpr int kCameraStillFrames = 10;
constexpr int kCameraWaitFrames = 150;  // about 5 s
int g_resumed_slot = -1;   // the slot last started at its point, and when
std::chrono::steady_clock::time_point g_resumed_at;
bool g_snapshot_pending = false;  // a save to keep once the EEPROM write is done
int g_snapshot_scene = 0;
int g_snapshot_slot = 0;
std::time_t g_snapshot_time = 0;

bool snapshots_enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("CONKER_SAVE_SNAPSHOTS");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}

std::filesystem::path points_path() {
    std::filesystem::path path = ultramodern::get_save_file_path();
    path.replace_extension(".resume");
    return path;
}

// One line per slot: "<slot> <counter> <scene> <entrance> <x> <y> <z> <yaw>", then "<facing> <camera
// yaw>" if they were recorded (points saved before that have none). `first_slot` is set
// to the slot of the first line, or -1 when there is none. Read with sscanf: a stream refuses a
// float too small to be normal (a facing of 5.7e-39 degrees), and the line would be lost.
std::array<Point, kSlots> read_points(const std::filesystem::path& path, int* first_slot = nullptr) {
    std::array<Point, kSlots> points{};
    if (first_slot != nullptr) {
        *first_slot = -1;
    }
    std::ifstream file(path);
    for (std::string line; std::getline(file, line);) {
        int slot;
        Point point;
        const int fields = std::sscanf(line.c_str(), "%d %u %d %d %f %f %f %f %f %f", &slot, &point.counter, &point.scene,
                                       &point.entrance, &point.x, &point.y, &point.z, &point.yaw, &point.facing,
                                       &point.camera_yaw);
        if (fields >= 8 && slot >= 0 && slot < kSlots) {
            point.valid = true;
            point.has_view = fields == 10;
            points[slot] = point;
            if (first_slot != nullptr && *first_slot < 0) {
                *first_slot = slot;
            }
        }
    }
    return points;
}

std::array<Point, kSlots> read_points() { return read_points(points_path()); }

// `first_slot`'s line goes first, so a snapshot says which slot it saved.
void write_points(const std::array<Point, kSlots>& points, const std::filesystem::path& path, int first_slot = -1) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::trunc);
    file << std::fixed << std::setprecision(2);
    auto write = [&](int slot) {
        const Point& p = points[slot];
        if (p.valid) {
            file << slot << ' ' << p.counter << ' ' << p.scene << ' ' << p.entrance << ' ' << p.x << ' ' << p.y << ' '
                 << p.z << ' ' << p.yaw;
            if (p.has_view) {
                file << ' ' << p.facing << ' ' << p.camera_yaw;
            }
            file << '\n';
        }
    };
    if (first_slot >= 0 && first_slot < kSlots) {
        write(first_slot);
    }
    for (int slot = 0; slot < kSlots; slot++) {
        if (slot != first_slot) {
            write(slot);
        }
    }
}

void write_points(const std::array<Point, kSlots>& points) { write_points(points, points_path()); }

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

// Puts Conker's object at the point, this frame and the last (so he does not sweep there).
void place(uint8_t* rdram, uint32_t object, const Point& point) {
    const float position[3] = {point.x, point.y, point.z};
    for (int i = 0; i < 3; i++) {
        write_float(rdram, object + kObjectX + 4 * i, position[i]);
        write_float(rdram, object + kObjectPreviousX + 4 * i, position[i]);
    }
    write_float(rdram, object + kObjectYaw, point.yaw * (3.14159265f / 180.0f));
}

uint32_t player_object(uint8_t* rdram) {
    const uint32_t object = static_cast<uint32_t>(MEM_W(0, kseg0(kPlayerObject)));
    return (object & 0xFF000000) == 0x80000000 ? object : 0;
}

// Measures how far the camera is from the point it looks at, and its pitch, for place_view.
void measure_camera(uint8_t* rdram) {
    Camera camera;
    if (current_camera(rdram, &camera)) {
        const float d[3] = {camera.at[0] - camera.eye[0], camera.at[1] - camera.eye[1], camera.at[2] - camera.eye[2]};
        g_camera_distance = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        g_camera_pitch = camera.pitch();
    }
}

// Turns Conker as the view says, and puts the camera where it looks along the view's yaw at him
// where he is.
void place_view(uint8_t* rdram, uint32_t object, const Position& view, bool turn_conker = true, bool camera_too = true) {
    if (turn_conker) {
        write_float(rdram, object + kObjectFacing, view.facing);
        const auto angle =
            static_cast<int16_t>(static_cast<int32_t>(std::lround((view.facing - 90.0f) * (65536.0f / 360.0f))));
        MEM_H(0, kseg0(object + kObjectFacingAngle)) = angle;
        MEM_H(0, kseg0(object + kObjectFacingTarget)) = angle;
    }
    if (!camera_too) {
        return;
    }

    const float yaw = view.camera_yaw * (3.14159265f / 180.0f);
    const float pitch = g_camera_pitch * (3.14159265f / 180.0f);
    const float forward[3] = {std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw)};
    const float at[3] = {read_float(rdram, object + kObjectX), read_float(rdram, object + kObjectY) + kLookAtHeight,
                         read_float(rdram, object + kObjectZ)};
    float eye[3];
    for (int i = 0; i < 3; i++) {
        eye[i] = at[i] - forward[i] * g_camera_distance;
    }
    const uint32_t camera = static_cast<uint32_t>(MEM_W(0, kseg0(object + kObjectCamera)));
    if ((camera & 0xFF800000) == 0x80000000) {
        for (int i = 0; i < 3; i++) {
            write_float(rdram, camera + kCameraForward + 4 * i, forward[i]);
            write_float(rdram, camera + kCameraEye + 4 * i, eye[i]);
            write_float(rdram, camera + kCameraAt + 4 * i, at[i]);
        }
    }
    const uint32_t control = static_cast<uint32_t>(MEM_W(0, kseg0(object + kObjectCameraControl)));
    if ((control & 0xFF800000) != 0x80000000) {
        return;
    }
    for (int copy : kControlEyes) {
        for (int i = 0; i < 3; i++) {
            write_float(rdram, control + copy + 4 * i, eye[i]);
        }
    }
    const float orbit = std::fmod(view.camera_yaw + 180.0f, 360.0f);
    const float behind = std::fmod(view.facing - view.camera_yaw + 360.0f, 360.0f);
    write_float(rdram, control + kOrbitDegrees, orbit);
    write_float(rdram, control + kOrbitTargetDegrees, orbit);
    write_float(rdram, control + kCameraBehind, behind);
    write_float(rdram, control + kOrbitRadians, orbit * (3.14159265f / 180.0f));
    write_float(rdram, control + kOrbitTargetRadians, orbit * (3.14159265f / 180.0f));
}

uint32_t save_counter(uint8_t* rdram, int slot) {
    return static_cast<uint32_t>(MEM_W(0, kseg0(kSaveCounters + static_cast<uint32_t>(slot) * 16)));
}

// saves/snapshots/scene<scene>-<YYYYMMDD>-<HHMMSS>, with -2, -3... for more in the same second.
std::filesystem::path snapshots_folder() { return ultramodern::get_save_file_path().parent_path() / "snapshots"; }

std::filesystem::path snapshot_path(int scene, std::time_t time) {
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);
    const std::filesystem::path folder = snapshots_folder();
    const std::string name = "scene" + std::to_string(scene) + "-" + stamp;
    std::filesystem::path path = folder / (name + ".bin");
    for (int n = 2; std::filesystem::exists(path); n++) {
        path = folder / (name + "-" + std::to_string(n) + ".bin");
    }
    return path;
}

void write_snapshot(uint8_t* rdram) {
    save_read(rdram, static_cast<int32_t>(kSnapshotScratch), 0, kEepromSize);
    std::array<char, kEepromSize> image;
    for (uint32_t i = 0; i < kEepromSize; i++) {
        image[i] = static_cast<char>(MEM_BU(i, kseg0(kSnapshotScratch)));
    }
    const std::filesystem::path path = snapshot_path(g_snapshot_scene, g_snapshot_time);
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.write(image.data(), image.size())) {
        std::printf("[conker] could not write save snapshot %s\n", path.string().c_str());
        return;
    }
    std::filesystem::path points = path;
    points.replace_extension(".resume");
    write_points(read_points(), points, g_snapshot_slot);
    std::printf("[conker] save snapshot: %s\n", path.string().c_str());
}

// After a resumed save has placed Conker: once the camera has stopped moving, puts it where it looked
// when the save was made (Conker turned again too, if he has not moved since) and holds it there.
void settle_camera(uint8_t* rdram) {
    if (!g_camera_waits || g_resuming.valid) {
        return;
    }
    const uint32_t object = player_object(rdram);
    Camera camera;
    if (object == 0 || !current_camera(rdram, &camera)) {
        if (++g_camera_wait > kCameraWaitFrames) {
            g_camera_waits = false;
        }
        return;
    }
    const float dx = read_float(rdram, object + kObjectX) - g_view.x;
    const float dz = read_float(rdram, object + kObjectZ) - g_view.z;
    const bool where_saved = dx * dx + dz * dz < 1.0f;
    if (g_camera_hold > 0) {
        place_view(rdram, object, g_view, where_saved, true);
        if (--g_camera_hold == 0) {
            g_camera_waits = false;
        }
        return;
    }
    float moved = 0;
    for (int i = 0; i < 3; i++) {
        moved += std::fabs(camera.eye[i] - g_last_eye[i]);
        g_last_eye[i] = camera.eye[i];
    }
    g_camera_still = moved < 0.5f ? g_camera_still + 1 : 0;
    if (g_camera_still >= kCameraStillFrames || ++g_camera_wait > kCameraWaitFrames) {
        measure_camera(rdram);
        place_view(rdram, object, g_view, where_saved, true);
        g_camera_hold = kPlaceFrames;
        std::printf("[conker] resumed camera: yaw %.2f\n", g_view.camera_yaw);
    }
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
    Position here;
    g_pending.has_view = current_position(rdram, &here) && here.has_view;
    g_pending.facing = here.facing;
    g_pending.camera_yaw = here.camera_yaw;
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

bool current_position(uint8_t* rdram, Position* out) {
    const uint32_t object = player_object(rdram);
    if (object == 0) {
        return false;
    }
    out->scene = MEM_W(0, kseg0(kScene));
    out->x = read_float(rdram, object + kObjectX);
    out->y = read_float(rdram, object + kObjectY);
    out->z = read_float(rdram, object + kObjectZ);
    out->yaw = read_float(rdram, object + kObjectYaw) * (180.0f / 3.14159265f);
    out->facing = std::fmod(read_float(rdram, object + kObjectFacing), 360.0f);
    if (out->facing < 0) {
        out->facing += 360.0f;
    }
    Camera camera;
    out->has_view = current_camera(rdram, &camera);
    out->camera_yaw = out->has_view ? camera.yaw() : 0.0f;
    out->camera_pitch = out->has_view ? camera.pitch() : 0.0f;
    return true;
}

float Camera::yaw() const {
    float degrees = std::atan2(forward[0], forward[2]) * (180.0f / 3.14159265f);
    return degrees < 0 ? degrees + 360.0f : degrees;
}

float Camera::pitch() const {
    return std::asin(std::clamp(forward[1], -1.0f, 1.0f)) * (180.0f / 3.14159265f);
}

bool current_camera(uint8_t* rdram, Camera* out) {
    const uint32_t object = player_object(rdram);
    if (object == 0) {
        return false;
    }
    const uint32_t camera = static_cast<uint32_t>(MEM_W(0, kseg0(object + kObjectCamera)));
    if ((camera & 0xFF800000) != 0x80000000) {  // in the game's 8 MiB
        return false;
    }
    for (int i = 0; i < 3; i++) {
        out->forward[i] = read_float(rdram, camera + kCameraForward + 4 * i);
        out->eye[i] = read_float(rdram, camera + kCameraEye + 4 * i);
        out->at[i] = read_float(rdram, camera + kCameraAt + 4 * i);
    }
    const float length = std::sqrt(out->forward[0] * out->forward[0] + out->forward[1] * out->forward[1] +
                                   out->forward[2] * out->forward[2]);
    return std::isfinite(length) && std::fabs(length - 1.0f) < 0.01f;
}

bool teleport(uint8_t* rdram, const Position& to) {
    const uint32_t object = player_object(rdram);
    if (object == 0 || MEM_W(0, kseg0(kScene)) != to.scene) {
        return false;
    }
    Point point;
    point.valid = true;
    point.scene = to.scene;
    point.x = to.x;
    point.y = to.y;
    point.z = to.z;
    point.yaw = to.yaw;
    place(rdram, object, point);
    if (to.has_view) {
        g_view = to;
        g_camera_waits = false;
        measure_camera(rdram);
        place_view(rdram, object, g_view);
    } else {
        g_view.has_view = false;
    }
    // Held there for a few frames more, through the end of the pause.
    g_resuming = point;
    g_resume_frames = kSettleFrames;
    g_place_frames = kTeleportFrames;
    if (to.has_view) {
        std::printf("[conker] teleport: scene %d to (%.2f, %.2f, %.2f) facing %.2f, camera yaw %.2f\n", to.scene, to.x, to.y,
                    to.z, to.facing, to.camera_yaw);
    } else {
        std::printf("[conker] teleport: scene %d to (%.2f, %.2f, %.2f)\n", to.scene, to.x, to.y, to.z);
    }
    return true;
}

std::vector<Snapshot> list_snapshots() {
    std::vector<Snapshot> snapshots;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(snapshots_folder(), error)) {
        const std::filesystem::path& path = entry.path();
        // scene<scene>-<YYYYMMDD>-<HHMMSS>[-<n>].bin
        int scene, n = 1;
        char date[9] = {}, time[7] = {};
        const std::string name = path.stem().string();
        if (path.extension() != ".bin" ||
            std::sscanf(name.c_str(), "scene%d-%8[0-9]-%6[0-9]-%d", &scene, date, time, &n) < 3 ||
            std::strlen(date) != 8 || std::strlen(time) != 6) {
            continue;
        }
        Snapshot snapshot;
        snapshot.path = path;
        snapshot.scene = scene;
        snapshot.when = std::string(date, 4) + "-" + std::string(date + 4, 2) + "-" + std::string(date + 6, 2) + " " +
                        std::string(time, 2) + ":" + std::string(time + 2, 2) + ":" + std::string(time + 4, 2);
        snapshot.order = std::string(date) + time + (n < 10 ? "0" : "") + std::to_string(n);
        snapshots.push_back(snapshot);
    }
    std::sort(snapshots.begin(), snapshots.end(), [](const Snapshot& a, const Snapshot& b) { return a.order > b.order; });
    return snapshots;
}

bool load_snapshot(uint8_t* rdram, const std::filesystem::path& path) {
    if (MEM_B(0, kseg0(kEepromAction)) != 0 || MEM_W(0, kseg0(kEepromWriting)) != 0 || g_snapshot_pending) {
        return false;  // a save is still being written
    }
    std::array<char, kEepromSize> image;
    std::ifstream file(path, std::ios::binary);
    if (!file.read(image.data(), image.size())) {
        std::printf("[conker] could not read save snapshot %s\n", path.string().c_str());
        return false;
    }
    for (uint32_t i = 0; i < kEepromSize; i++) {
        MEM_B(i, kseg0(kSnapshotScratch)) = static_cast<int8_t>(image[i]);
    }
    save_write(rdram, static_cast<int32_t>(kSnapshotScratch), 0, kEepromSize);
    std::filesystem::path points = path;
    points.replace_extension(".resume");
    int slot;
    const std::array<Point, kSlots> snapshot_points = read_points(points, &slot);
    write_points(snapshot_points, points_path(), slot);
    if (slot < 0) {
        slot = MEM_B(0, kseg0(kSaveSlot)) >= 0 ? MEM_B(0, kseg0(kSaveSlot)) : 0;
    } else {
        // The slot's sequence number as the game last read or wrote it, which starting the save
        // checks the point against (conker_resume_start): the snapshot's.
        MEM_W(0, kseg0(kSaveCounters + static_cast<uint32_t>(slot) * 16)) =
            static_cast<int32_t>(snapshot_points[slot].counter);
    }
    MEM_B(0, kseg0(kSaveSlot)) = static_cast<int8_t>(slot);
    request_load(rdram, slot);
    std::printf("[conker] loading save snapshot %s, slot %d\n", path.string().c_str(), slot);
    return true;
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
    if (snapshots_enabled()) {
        g_snapshot_pending = true;
        g_snapshot_scene = g_pending.scene;
        g_snapshot_slot = slot;
        g_snapshot_time = std::time(nullptr);
    }
    std::printf("[conker] resume point: slot %d, save %u, scene %d entrance %d at (%.2f, %.2f, %.2f) facing %.2f, camera "
                "yaw %.2f\n",
                slot, points[slot].counter, g_pending.scene, g_pending.entrance, g_pending.x, g_pending.y, g_pending.z,
                g_pending.facing, g_pending.camera_yaw);
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
    g_place_frames = kPlaceFrames;
    g_view = Position{};
    if (point.has_view) {
        g_view.scene = point.scene;
        g_view.x = point.x;
        g_view.y = point.y;
        g_view.z = point.z;
        g_view.yaw = point.yaw;
        g_view.has_view = true;
        g_view.facing = point.facing;
        g_view.camera_yaw = point.camera_yaw;
        g_camera_waits = true;
        g_camera_still = 0;
        g_camera_wait = 0;
        g_camera_hold = 0;
    }
    g_resumed_slot = slot;
    g_resumed_at = std::chrono::steady_clock::now();
    if (point.has_view) {
        std::printf("[conker] resuming slot %d in scene %d at (%.2f, %.2f, %.2f) facing %.2f, camera yaw %.2f\n", slot,
                    point.scene, point.x, point.y, point.z, point.facing, point.camera_yaw);
    } else {
        std::printf("[conker] resuming slot %d in scene %d at (%.0f, %.0f, %.0f)\n", slot, point.scene, point.x, point.y,
                    point.z);
    }
}

// At the start of func_1501C57C, the EEPROM manager, once a frame: when a LOAD's slot read has
// been done, start the game as PLAY does.
// Also keeps the snapshot of a save once it is written, and moves Conker to the point being
// resumed once its scene is running.
extern "C" void conker_resume_tick(uint8_t* rdram, recomp_context* ctx) {
    if (g_snapshot_pending && MEM_B(0, kseg0(kEepromAction)) == 0 && MEM_W(0, kseg0(kEepromWriting)) == 0) {
        g_snapshot_pending = false;
        write_snapshot(rdram);
    }
    if (g_load_requested && MEM_B(0, kseg0(kEepromAction)) == 0) {
        g_load_requested = false;
        recomp_context saved = *ctx;
        func_151E327C(rdram, ctx);
        *ctx = saved;
        return;
    }
    settle_camera(rdram);
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
    place(rdram, object, g_resuming);
    if (g_view.has_view) {
        place_view(rdram, object, g_view, true, !g_camera_waits);
    }
    if (g_resume_frames >= kSettleFrames + g_place_frames) {
        g_resuming.valid = false;
    }
}
