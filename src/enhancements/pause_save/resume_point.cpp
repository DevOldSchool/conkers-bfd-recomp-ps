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
// The game's save holds finished events (the event flags) and what has been collected, but not
// what is under way in the scene: a conversation partway through a chain (the event script's
// variables) or what a script has done to the scene's objects (a gate it opened). On hardware that
// is lost by design, as a save always restarts at a checkpoint. So the point also records the
// scene's objects' states and rotations and the running event scripts' variables, and puts them
// back as Conker is placed.
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
#include <sstream>
#include <string>

#include "build_version.h"
#include "json/json.hpp"
#include "recomp.h"
#include "ultramodern/ultramodern.hpp"
#include "enhancements/status.hpp"

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
constexpr int8_t kEepromSave = 2;
constexpr uint32_t kChapterReplay = 0x800BE616;  // D_800BE616: playing from CHAPTERS, saves off
constexpr std::chrono::seconds kReportSaveWait{5};  // a report's save not written by then: none
constexpr uint32_t kEepromWriting = 0x800E0A20;  // D_800E0A20: paced EEPROM write under way, 0 when none
constexpr uint32_t kEepromSize = 0x800;          // 16 kbit
constexpr uint32_t kSnapshotScratch = 0x807F9000;  // free RDRAM, see pause_save.cpp
constexpr uint32_t kReportBlockScratch = 0x807F9800;  // 8 bytes after it
constexpr int kSlots = 3;
// A save the game makes this soon after resuming (it saves on entering some areas) is still the
// resumed one: the point carries over to it.
constexpr std::chrono::seconds kResumeSaveWindow{10};

// The scene's objects: D_800DBEF0 records of 0xA0 bytes at D_800DBEF4 (a gate, a door, a spinning
// pickup). The state byte drives the object: the gate in the cheese farm (scene 12) is 4 shut and
// 7 open, and swings to match it. Byte 0x71 is the record's own index.
constexpr uint32_t kObjectCount = 0x800DBEF0;
constexpr uint32_t kObjects = 0x800DBEF4;
constexpr uint32_t kObjectRecordSize = 0xA0;
constexpr int kObjectRotation = 0x00;            // three floats, degrees
constexpr int kObjectIndex = 0x71;
constexpr int kObjectState = 0x73;
constexpr int kMaxObjects = 512;
// The running event scripts: a list {u16 count, head, tail} of nodes {u16 key (event id in the low
// 10 bits), ..., u16 +6 variables' size, u16 +0xA their offset in the node, ..., next +0x18}.
constexpr uint32_t kScriptCount = 0x800D2F48;
constexpr uint32_t kScriptHead = 0x800D2F4C;
constexpr int kScriptVariablesSize = 0x6;
constexpr int kScriptVariablesOffset = 0xA;
constexpr int kScriptNext = 0x18;
constexpr uint16_t kScriptEvent = 0x3FF;
constexpr int kMaxScripts = 64;
constexpr size_t kMaxScriptVariables = 1024;

struct SceneObject {
    int index = 0;
    int state = 0;
    float rotation[3] = {};
};

struct ScriptVariables {
    uint16_t key = 0;
    std::vector<uint8_t> bytes;
};

struct Point {
    bool valid = false;
    uint32_t counter = 0;  // the save's sequence number
    int scene = 0;
    int entrance = 0;
    float x = 0, y = 0, z = 0, yaw = 0;  // yaw in degrees
    bool has_view = false;               // facing and camera_yaw recorded (see Position)
    float facing = 0, camera_yaw = 0;
    std::vector<SceneObject> objects;    // the scene as it was (see the top of the file)
    std::vector<ScriptVariables> scripts;
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
// A debug report being made (request_report). Its save is the game's own, run as SAVE runs it, but
// the EEPROM writes are caught (conker_report_eeprom_write) into a copy of the save instead, so the
// player's save is not touched.
struct PendingReport {
    bool active = false;
    std::filesystem::path folder;
    std::string text;
    std::chrono::steady_clock::time_point requested;
    bool capturing = false;  // the save's EEPROM writes go to `image`
    bool saved = false;      // the game has made the save (conker_resume_saved)
    int slot = 0;
    int32_t counter_before = 0;  // the slot's sequence number in RAM, put back afterwards
    std::array<char, kEepromSize> image{};
    std::array<Point, kSlots> points{};
};
PendingReport g_report;
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

int hex_digit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

// A line of the scene's state under a slot's point, added to `point`: "object <index> <state> <rotation
// x> <y> <z>" or "script <key, hex> <variables, hex>". False when the line is neither, or out of range.
bool read_scene_line(const std::string& line, Point* point) {
    int index, state, consumed = 0;
    SceneObject object;
    if (std::sscanf(line.c_str(), "object %d %d %f %f %f%n", &index, &state, &object.rotation[0], &object.rotation[1],
                    &object.rotation[2], &consumed) == 5 &&
        consumed == static_cast<int>(line.size())) {
        if (index < 0 || index >= kMaxObjects || state < 0 || state > 0xFF ||
            point->objects.size() >= static_cast<size_t>(kMaxObjects)) {
            return false;
        }
        for (float value : object.rotation) {
            if (!std::isfinite(value) || std::fabs(value) > 1.0e6f) {
                return false;
            }
        }
        object.index = index;
        object.state = state;
        point->objects.push_back(object);
        return true;
    }
    unsigned key;
    char bytes[2 * kMaxScriptVariables + 2] = {};
    char format[32];
    std::snprintf(format, sizeof(format), "script %%x %%%zu[0-9a-fA-F]%%n", sizeof(bytes) - 1);
    consumed = 0;
    if (std::sscanf(line.c_str(), format, &key, bytes, &consumed) != 2 || consumed != static_cast<int>(line.size()) ||
        key > 0xFFFF || point->scripts.size() >= static_cast<size_t>(kMaxScripts)) {
        return false;
    }
    const size_t digits = std::strlen(bytes);
    if (digits % 2 != 0 || digits > 2 * kMaxScriptVariables) {
        return false;
    }
    ScriptVariables script;
    script.key = static_cast<uint16_t>(key);
    for (size_t i = 0; i < digits; i += 2) {
        script.bytes.push_back(static_cast<uint8_t>(hex_digit(bytes[i]) * 16 + hex_digit(bytes[i + 1])));
    }
    point->scripts.push_back(std::move(script));
    return true;
}

// One line per slot: "<slot> <counter> <scene> <entrance> <x> <y> <z> <yaw>", then "<facing> <camera
// yaw>" if they were recorded, followed by the scene's state as it was (read_scene_line). `first_slot`
// is set to the slot of the first point, or -1 when there is none. Read with sscanf: a stream refuses
// a float too small to be normal (a facing of 5.7e-39 degrees), and the line would be lost.
std::array<Point, kSlots> read_points(const std::filesystem::path& path, int* first_slot = nullptr) {
    std::array<Point, kSlots> points{};
    if (first_slot != nullptr) {
        *first_slot = -1;
    }
    Point* last = nullptr;
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
            last = &points[slot];
            if (first_slot != nullptr && *first_slot < 0) {
                *first_slot = slot;
            }
        } else if (last != nullptr) {
            read_scene_line(line, last);
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
            for (const SceneObject& object : p.objects) {
                file << "object " << object.index << ' ' << object.state << ' ' << object.rotation[0] << ' '
                     << object.rotation[1] << ' ' << object.rotation[2] << '\n';
            }
            for (const ScriptVariables& script : p.scripts) {
                char key[8];
                std::snprintf(key, sizeof(key), "%x", script.key);
                file << "script " << key << ' ';
                for (uint8_t byte : script.bytes) {
                    char digits[3];
                    std::snprintf(digits, sizeof(digits), "%02x", byte);
                    file << digits;
                }
                file << '\n';
            }
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

bool in_rdram(uint32_t address) { return (address & 0xFF800000) == 0x80000000; }

// The scene's object records and how many there are; 0 when there is no scene.
uint32_t scene_objects(uint8_t* rdram, int* count) {
    const uint32_t table = static_cast<uint32_t>(MEM_W(0, kseg0(kObjects)));
    *count = MEM_W(0, kseg0(kObjectCount));
    if (!in_rdram(table) || *count <= 0 || *count > kMaxObjects) {
        *count = 0;
        return 0;
    }
    return table;
}

// Calls `visit(node)` for each running event script, while the list holds together.
template <typename Visit>
void for_each_script(uint8_t* rdram, Visit visit) {
    const int count = MEM_HU(0, kseg0(kScriptCount));
    uint32_t node = static_cast<uint32_t>(MEM_W(0, kseg0(kScriptHead)));
    for (int i = 0; i < count && i < kMaxScripts && in_rdram(node); i++) {
        visit(node);
        node = static_cast<uint32_t>(MEM_W(0, kseg0(node + kScriptNext)));
    }
}

// Records the scene as it is now (see the top of the file) in `point`.
void capture_scene(uint8_t* rdram, Point* point) {
    point->objects.clear();
    point->scripts.clear();
    int count;
    const uint32_t table = scene_objects(rdram, &count);
    for (int index = 0; index < count; index++) {
        const uint32_t record = table + static_cast<uint32_t>(index) * kObjectRecordSize;
        SceneObject object;
        object.index = index;
        object.state = MEM_BU(kObjectState, kseg0(record));
        for (int i = 0; i < 3; i++) {
            object.rotation[i] = read_float(rdram, record + kObjectRotation + 4 * i);
        }
        point->objects.push_back(object);
    }
    for_each_script(rdram, [&](uint32_t node) {
        const uint32_t size = MEM_HU(kScriptVariablesSize, kseg0(node));
        const uint32_t variables = node + MEM_HU(kScriptVariablesOffset, kseg0(node));
        if (size == 0 || size > kMaxScriptVariables) {
            return;
        }
        ScriptVariables script;
        script.key = MEM_HU(0, kseg0(node));
        for (uint32_t i = 0; i < size; i++) {
            script.bytes.push_back(MEM_BU(static_cast<int>(i), kseg0(variables)));
        }
        point->scripts.push_back(std::move(script));
    });
}

// Puts back the scene `point` recorded: each object's state and rotation (when the record is the
// one recorded), and the variables of each event script running again (the same event, the same
// size). The game then moves the objects to match their states, as it does when a script sets them.
void restore_scene(uint8_t* rdram, const Point& point) {
    int count;
    const uint32_t table = scene_objects(rdram, &count);
    int objects = 0, scripts = 0;
    for (const SceneObject& object : point.objects) {
        const uint32_t record = table + static_cast<uint32_t>(object.index) * kObjectRecordSize;
        if (object.index >= count || MEM_BU(kObjectIndex, kseg0(record)) != (object.index & 0xFF)) {
            continue;
        }
        MEM_B(kObjectState, kseg0(record)) = static_cast<int8_t>(object.state);
        for (int i = 0; i < 3; i++) {
            write_float(rdram, record + kObjectRotation + 4 * i, object.rotation[i]);
        }
        objects++;
    }
    for_each_script(rdram, [&](uint32_t node) {
        const uint16_t key = MEM_HU(0, kseg0(node));
        const uint32_t size = MEM_HU(kScriptVariablesSize, kseg0(node));
        const uint32_t variables = node + MEM_HU(kScriptVariablesOffset, kseg0(node));
        for (const ScriptVariables& script : point.scripts) {
            if ((script.key & kScriptEvent) == (key & kScriptEvent) && script.bytes.size() == size) {
                for (uint32_t i = 0; i < size; i++) {
                    MEM_B(static_cast<int>(i), kseg0(variables)) = static_cast<int8_t>(script.bytes[i]);
                }
                scripts++;
                break;
            }
        }
    });
    std::printf("[conker] resumed scene state: %d of %zu objects, %d of %zu event scripts\n", objects,
                point.objects.size(), scripts, point.scripts.size());
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

std::filesystem::path reports_folder() {
    return data_folder() / "debug_reports";
}

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

// The save as it is now, as conker_us.bin and conker_us.resume hold it: `path` and the .resume next
// to it (the saved slot's line first).
bool write_save_copy(const std::array<char, kEepromSize>& image, const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.write(image.data(), image.size())) {
        std::printf("[conker] could not write %s\n", path.string().c_str());
        return false;
    }
    std::filesystem::path points = path;
    points.replace_extension(".resume");
    write_points(read_points(), points, g_snapshot_slot);
    return true;
}

// report.json: the report the debug menu made, with "save" saying what save is in the folder.
void finish_report(const nlohmann::json& save) {
    std::error_code error;
    std::filesystem::create_directories(g_report.folder, error);
    nlohmann::json report = nlohmann::json::parse(g_report.text, nullptr, false);
    if (!report.is_object()) {
        report = nlohmann::json::object();
    }
    report["save"] = save;
    std::ofstream(g_report.folder / "report.json", std::ios::trunc) << report.dump(2) << '\n';
    std::printf("[conker] debug report: %s\n", g_report.folder.string().c_str());
    g_report.active = false;
}

nlohmann::json no_save(const char* reason) { return {{"slot", nullptr}, {"reason", reason}}; }

void write_snapshot(uint8_t* rdram) {
    save_read(rdram, static_cast<int32_t>(kSnapshotScratch), 0, kEepromSize);
    std::array<char, kEepromSize> image;
    for (uint32_t i = 0; i < kEepromSize; i++) {
        image[i] = static_cast<char>(MEM_BU(i, kseg0(kSnapshotScratch)));
    }
    if (snapshots_enabled()) {
        const std::filesystem::path path = snapshot_path(g_snapshot_scene, g_snapshot_time);
        if (write_save_copy(image, path)) {
            std::printf("[conker] save snapshot: %s\n", path.string().c_str());
        }
    }
}

// The report's save, once the game has made it: save.bin, save.resume and report.json.
void write_report_save() {
    std::error_code error;
    std::filesystem::create_directories(g_report.folder, error);
    std::ofstream file(g_report.folder / "save.bin", std::ios::binary | std::ios::trunc);
    if (!file.write(g_report.image.data(), g_report.image.size())) {
        finish_report(no_save("could not be written"));
        return;
    }
    file.close();
    write_points(g_report.points, g_report.folder / "save.resume", g_report.slot);
    finish_report({{"slot", g_report.slot},
                   {"counter", g_report.points[g_report.slot].counter},
                   {"files", {"save.bin", "save.resume"}},
                   {"note", "made for this report; the player's own save was not changed"}});
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
    capture_scene(rdram, &g_pending);
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

namespace {

// Reads "scene<scene>-<YYYYMMDD>-<HHMMSS>[-<n>]" into a Snapshot's scene, when and order.
bool parse_name(const std::string& name, Snapshot* out) {
    int scene, n = 1;
    char date[9] = {}, time[7] = {};
    if (std::sscanf(name.c_str(), "scene%d-%8[0-9]-%6[0-9]-%d", &scene, date, time, &n) < 3 || std::strlen(date) != 8 ||
        std::strlen(time) != 6) {
        return false;
    }
    out->scene = scene;
    out->when = std::string(date, 4) + "-" + std::string(date + 4, 2) + "-" + std::string(date + 6, 2) + " " +
                std::string(time, 2) + ":" + std::string(time + 2, 2) + ":" + std::string(time + 4, 2);
    out->order = std::string(date) + time + (n < 10 ? "0" : "") + std::to_string(n);
    return true;
}

void newest_first(std::vector<Snapshot>* list) {
    std::sort(list->begin(), list->end(), [](const Snapshot& a, const Snapshot& b) { return a.order > b.order; });
}

}  // namespace

std::filesystem::path data_folder() {
    // The save file's folder's folder, taken before a report's save can move the save file
    // (conker_resume_tick asks once a frame from the start).
    static const std::filesystem::path folder = ultramodern::get_save_file_path().parent_path().parent_path();
    return folder;
}

std::vector<Snapshot> list_snapshots() {
    std::vector<Snapshot> snapshots;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(snapshots_folder(), error)) {
        Snapshot snapshot;
        snapshot.path = entry.path();
        if (snapshot.path.extension() == ".bin" && parse_name(snapshot.path.stem().string(), &snapshot)) {
            snapshots.push_back(snapshot);
        }
    }
    newest_first(&snapshots);
    return snapshots;
}

std::vector<Snapshot> list_reports() {
    std::vector<Snapshot> reports;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(reports_folder(), error)) {
        Snapshot report;
        report.path = entry.path() / "save.bin";
        if (entry.is_directory() && std::filesystem::exists(entry.path() / "report.json") &&
            parse_name(entry.path().filename().string(), &report)) {
            reports.push_back(report);
        }
    }
    newest_first(&reports);
    return reports;
}

std::string request_report(uint8_t* rdram, const std::string& text) {
    if (g_report.active || g_snapshot_pending || MEM_B(0, kseg0(kEepromAction)) != 0 ||
        MEM_W(0, kseg0(kEepromWriting)) != 0) {
        return "";  // a save is still being written
    }
    Position here;
    current_position(rdram, &here);
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);
    const std::string name = "scene" + std::to_string(here.scene) + "-" + stamp;
    std::filesystem::path folder = reports_folder() / name;
    for (int n = 2; std::filesystem::exists(folder); n++) {
        folder = reports_folder() / (name + "-" + std::to_string(n));
    }
    g_report.active = true;
    g_report.folder = folder;
    g_report.text = text;
    g_report.requested = std::chrono::steady_clock::now();
    const int slot = MEM_B(0, kseg0(kSaveSlot));
    if (slot < 0 || slot >= kSlots || MEM_BU(0, kseg0(kChapterReplay)) != 0) {
        finish_report(no_save("no game slot, or a chapter replayed from CHAPTERS, where the game does not save"));
        return folder.filename().string();
    }
    // The save is made as SAVE in the pause menu makes it, into a copy of the save as it is now.
    save_read(rdram, static_cast<int32_t>(kSnapshotScratch), 0, kEepromSize);
    for (uint32_t i = 0; i < kEepromSize; i++) {
        g_report.image[i] = static_cast<char>(MEM_BU(i, kseg0(kSnapshotScratch)));
    }
    g_report.capturing = true;
    g_report.saved = false;
    g_report.slot = slot;
    g_report.counter_before = MEM_W(0, kseg0(kSaveCounters + static_cast<uint32_t>(slot) * 16));
    remember_position(rdram, slot);
    MEM_B(0, kseg0(kEepromAction)) = kEepromSave;
    MEM_B(0, kseg0(kEepromSlot)) = static_cast<int8_t>(slot);
    return folder.filename().string();
}

namespace {
void start_save(uint8_t* rdram, const std::array<Point, kSlots>& points, int slot);
}  // namespace

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
    start_save(rdram, snapshot_points, slot);
    std::printf("[conker] loading save snapshot %s, slot %d\n", path.string().c_str(), MEM_B(0, kseg0(kSaveSlot)));
    return true;
}

namespace {

constexpr const char* kReportFormat = "conker-recomp-debug-report";
constexpr int kReportVersion = 1;
constexpr uintmax_t kMaxReportJson = 256 * 1024;
constexpr uintmax_t kMaxReportPoints = 512 * 1024;  // three slots of points with their scene state
constexpr double kMaxCoordinate = 1.0e6;   // well past any level's size
constexpr int kMaxScene = 255;

// A report's files, read once: what is checked is what is loaded.
struct ReportFiles {
    nlohmann::json report;
    std::array<char, kEepromSize> save{};
    std::string points;
};

// Reads `name` from `folder` into `out`: a regular file (not a link) of at most `limit` bytes, or
// exactly `limit` bytes if `exact`.
bool read_report_file(const std::filesystem::path& folder, const char* name, uintmax_t limit, bool exact,
                      std::string* out, std::string* problem) {
    const std::filesystem::path path = folder / name;
    std::error_code error;
    if (std::filesystem::is_symlink(path, error) || !std::filesystem::is_regular_file(path, error)) {
        *problem = std::string("no ") + name;
        return false;
    }
    const uintmax_t size = std::filesystem::file_size(path, error);
    if (error || (exact ? size != limit : size > limit)) {
        *problem = std::string(name) + " is the wrong size";
        return false;
    }
    std::ifstream file(path, std::ios::binary);
    out->assign(static_cast<size_t>(size), '\0');
    if (!file.read(out->data(), static_cast<std::streamsize>(size))) {
        *problem = std::string("cannot read ") + name;
        return false;
    }
    return true;
}

// A finite number in [low, high] at `key` of `object`.
bool number_in(const nlohmann::json& object, const char* key, double low, double high, double* out = nullptr) {
    if (!object.is_object() || !object.contains(key) || !object[key].is_number()) {
        return false;
    }
    const double value = object[key].get<double>();
    if (!std::isfinite(value) || value < low || value > high) {
        return false;
    }
    if (out != nullptr) {
        *out = value;
    }
    return true;
}

// Checks a report, which may come from anyone, before anything is loaded from it.
bool check_report(const std::filesystem::path& folder, ReportFiles* files, std::string* problem) {
    Snapshot name;
    std::error_code error;
    if (!parse_name(folder.filename().string(), &name) || std::filesystem::is_symlink(folder, error)) {
        *problem = "the folder is not a report";
        return false;
    }
    std::string text, save;
    if (!read_report_file(folder, "report.json", kMaxReportJson, false, &text, problem) ||
        !read_report_file(folder, "save.bin", kEepromSize, true, &save, problem) ||
        !read_report_file(folder, "save.resume", kMaxReportPoints, false, &files->points, problem)) {
        return false;
    }
    std::copy(save.begin(), save.end(), files->save.begin());

    nlohmann::json& report = files->report;
    report = nlohmann::json::parse(text, nullptr, false);
    if (!report.is_object()) {
        *problem = "report.json is not JSON";
        return false;
    }
    if (!report.contains("format") || report["format"] != kReportFormat || !report.contains("version") ||
        report["version"] != kReportVersion) {
        *problem = "report.json is not a version 1 report";
        return false;
    }
    double scene, x, y, z;
    const nlohmann::json conker = report.value("conker", nlohmann::json());
    if (!number_in(report, "scene", 0, kMaxScene, &scene) || scene != std::floor(scene) ||
        !number_in(report, "entrance", 0, 255) || !number_in(conker, "x", -kMaxCoordinate, kMaxCoordinate, &x) ||
        !number_in(conker, "y", -kMaxCoordinate, kMaxCoordinate, &y) ||
        !number_in(conker, "z", -kMaxCoordinate, kMaxCoordinate, &z) || !number_in(conker, "facing", -720, 720)) {
        *problem = "report.json has no valid scene and position";
        return false;
    }
    double slot, counter;
    const nlohmann::json save_info = report.value("save", nlohmann::json());
    if (!number_in(save_info, "slot", 0, kSlots - 1, &slot) || slot != std::floor(slot) ||
        !number_in(save_info, "counter", 0, 4294967295.0, &counter) || counter != std::floor(counter)) {
        *problem = "the report has no save";
        return false;
    }

    // save.resume: every line well formed (a point, or the scene's state under one), the saved
    // slot's point the report's.
    std::istringstream lines(files->points);
    std::array<bool, kSlots> seen{};
    bool found = false;
    int count = 0;
    Point scene_state;  // the current point's scene lines, checked as they are read
    for (std::string line; std::getline(lines, line);) {
        if (line.empty()) {
            continue;
        }
        if (line.compare(0, 7, "object ") == 0 || line.compare(0, 7, "script ") == 0) {
            if (count == 0 || !read_scene_line(line, &scene_state)) {
                *problem = "save.resume is not valid";
                return false;
            }
            continue;
        }
        scene_state = Point{};
        int line_slot;
        Point point;
        const int fields = std::sscanf(line.c_str(), "%d %u %d %d %f %f %f %f %f %f", &line_slot, &point.counter,
                                       &point.scene, &point.entrance, &point.x, &point.y, &point.z, &point.yaw,
                                       &point.facing, &point.camera_yaw);
        const float values[] = {point.x, point.y, point.z, point.yaw, point.facing, point.camera_yaw};
        bool sound = (fields == 8 || fields == 10) && ++count <= kSlots && line_slot >= 0 && line_slot < kSlots &&
                     !seen[line_slot] && point.scene >= 0 && point.scene <= kMaxScene && point.entrance >= 0 &&
                     point.entrance <= 255;
        for (float value : values) {
            sound = sound && std::isfinite(value) && std::fabs(value) <= kMaxCoordinate;
        }
        if (!sound) {
            *problem = "save.resume is not valid";
            return false;
        }
        seen[line_slot] = true;
        if (count == 1 && line_slot != static_cast<int>(slot)) {
            *problem = "save.resume does not start with the report's slot";  // the slot a load starts
            return false;
        }
        if (line_slot == static_cast<int>(slot)) {
            found = point.counter == static_cast<uint32_t>(counter) && point.scene == static_cast<int>(scene) &&
                    std::fabs(point.x - x) < 0.01 && std::fabs(point.y - y) < 0.01 && std::fabs(point.z - z) < 0.01;
        }
    }
    if (!found) {
        *problem = "save.resume does not match report.json";
        return false;
    }
    return true;
}

}  // namespace

bool load_report(uint8_t* rdram, const std::filesystem::path& path, std::string* problem) {
    if (MEM_B(0, kseg0(kEepromAction)) != 0 || MEM_W(0, kseg0(kEepromWriting)) != 0 || g_snapshot_pending ||
        g_report.active) {
        *problem = "saving, try again";
        return false;
    }
    const std::filesystem::path folder = path.parent_path();
    ReportFiles files;
    if (!check_report(folder, &files, problem)) {
        std::printf("[conker] debug report %s refused: %s\n", folder.string().c_str(), problem->c_str());
        return false;
    }
    const std::string build = files.report.value("build", std::string("unknown"));
    if (build != CONKER_BUILD_VERSION) {
        std::printf("[conker] debug report %s was made by build %s; this is %s\n", folder.string().c_str(),
                    build.c_str(), CONKER_BUILD_VERSION);
    }
    // The player's saves folder, as it was before any report was loaded.
    const std::filesystem::path saves = data_folder() / "saves";
    const std::string name = folder.filename().string();
    const std::filesystem::path copy = saves / "debug" / (name + ".bin");
    std::filesystem::path copy_points = copy;
    copy_points.replace_extension(".resume");
    std::error_code error;
    std::filesystem::create_directories(copy.parent_path(), error);
    std::ofstream save_file(copy, std::ios::binary | std::ios::trunc);
    std::ofstream points_file(copy_points, std::ios::trunc);
    if (!save_file.write(files.save.data(), files.save.size()) || !(points_file << files.points)) {
        *problem = "cannot copy the save";
        return false;
    }
    save_file.close();
    points_file.close();
    // The game now plays on the copy: its saves, and their snapshots, go there until it is closed.
    ultramodern::change_save_file(u8"debug", std::u8string(name.begin(), name.end()));
    int slot;
    const std::array<Point, kSlots> report_points = read_points(points_path(), &slot);
    start_save(rdram, report_points, slot);
    std::printf("[conker] loading debug report %s, slot %d, playing on %s until the game is closed\n",
                folder.string().c_str(), MEM_B(0, kseg0(kSaveSlot)), copy.string().c_str());
    return true;
}

namespace {

// Starts `slot` of the save just put in place, as LOAD does (the first point's slot if there is one).
void start_save(uint8_t* rdram, const std::array<Point, kSlots>& points, int slot) {
    if (slot < 0) {
        slot = MEM_B(0, kseg0(kSaveSlot)) >= 0 ? MEM_B(0, kseg0(kSaveSlot)) : 0;
    } else {
        // The slot's sequence number as the game last read or wrote it, which starting the save
        // checks the point against (conker_resume_start): the snapshot's.
        MEM_W(0, kseg0(kSaveCounters + static_cast<uint32_t>(slot) * 16)) = static_cast<int32_t>(points[slot].counter);
    }
    MEM_B(0, kseg0(kSaveSlot)) = static_cast<int8_t>(slot);
    request_load(rdram, slot);
}

}  // namespace

}  // namespace conker::resume

using namespace conker::resume;

// In func_15006BEC (write a save) once the slot's new sequence number is stored ($s1 slot,
// $s0 sequence number): if this save came from the pause menu, record where Conker was.
extern "C" void conker_resume_saved(uint8_t* /*rdram*/, recomp_context* ctx) {
    const int slot = static_cast<int32_t>(ctx->r17);
    const uint32_t counter = static_cast<uint32_t>(ctx->r16);
    if (g_report.capturing) {  // a report's save: its points go to the report, not conker_us.resume
        g_report.points = read_points();
        if (g_pending.valid && slot == g_pending_slot) {
            g_report.points[slot] = g_pending;
            g_report.points[slot].counter = counter;
        }
        g_report.slot = slot;
        g_report.saved = true;
        g_pending.valid = false;
        return;
    }
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
    data_folder();  // fixed from the first frame, before any report is loaded
    if (g_snapshot_pending && MEM_B(0, kseg0(kEepromAction)) == 0 && MEM_W(0, kseg0(kEepromWriting)) == 0) {
        g_snapshot_pending = false;
        write_snapshot(rdram);
    }
    if (g_report.capturing && (g_report.saved || std::chrono::steady_clock::now() - g_report.requested > kReportSaveWait) &&
        MEM_B(0, kseg0(kEepromAction)) == 0) {
        g_report.capturing = false;
        MEM_W(0, kseg0(kSaveCounters + static_cast<uint32_t>(g_report.slot) * 16)) = g_report.counter_before;
        if (g_report.saved) {
            write_report_save();
        } else {
            finish_report(no_save("the game did not make one"));
        }
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
    if (g_resume_frames == kSettleFrames + 1 && (!g_resuming.objects.empty() || !g_resuming.scripts.empty())) {
        restore_scene(rdram, g_resuming);
    }
    place(rdram, object, g_resuming);
    if (g_view.has_view) {
        place_view(rdram, object, g_view, true, !g_camera_waits);
    }
    if (g_resume_frames >= kSettleFrames + g_place_frames) {
        g_resuming.valid = false;
    }
}

extern "C" int conker_report_skip_write = 0;

// At the start of func_151DCEF0, which starts a paced EEPROM write of `size` bytes ($a3) from
// `buffer` ($a2) at 8-byte block `block` ($a1): while a debug report's save is being made, the bytes
// go into the report's copy of the save and the write is skipped (returning 0, as it does when it
// starts one).
extern "C" void conker_report_eeprom_write(uint8_t* rdram, recomp_context* ctx) {
    conker_report_skip_write = 0;
    if (!g_report.capturing) {
        return;
    }
    const uint32_t start = static_cast<uint32_t>(ctx->r5 & 0xFF) * 8;
    const uint32_t size = static_cast<uint32_t>(ctx->r7);
    for (uint32_t i = 0; i < size && start + i < kEepromSize; i++) {
        g_report.image[start + i] = static_cast<char>(MEM_BU(static_cast<int>(i), ctx->r6));
    }
    ctx->r2 = 0;
    conker_report_skip_write = 1;
}

// In func_15006BEC before `jal osEepromWrite` (0x15006D1C), which marks the slot's block
// slot * 16 + 0x13 before the slot is written ($a1 block, $a2 the 8 bytes): while a debug report's
// save is being made, the bytes go into the report's copy, and the write is pointed at the save's
// own bytes for that block, so it leaves the player's save as it was.
extern "C" void conker_report_marker_write(uint8_t* rdram, recomp_context* ctx) {
    if (!g_report.capturing) {
        return;
    }
    const uint32_t start = static_cast<uint32_t>(ctx->r5 & 0xFF) * 8;
    for (uint32_t i = 0; i < 8 && start + i < kEepromSize; i++) {
        g_report.image[start + i] = static_cast<char>(MEM_BU(static_cast<int>(i), ctx->r6));
    }
    save_read(rdram, static_cast<int32_t>(kReportBlockScratch), start, 8);
    ctx->r6 = kseg0(kReportBlockScratch);
}

nlohmann::json conker::status::save_snapshots() {
    return enhancement("save_snapshots", "Save snapshots", conker::resume::snapshots_enabled(), "CONKER_SAVE_SNAPSHOTS");
}
