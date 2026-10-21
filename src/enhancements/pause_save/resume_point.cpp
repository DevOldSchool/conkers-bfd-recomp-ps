// Exact-position saves. Native helpers called from the [[patches.hook]] snippets in config/us.toml
// (func_15006BEC, func_151E327C, func_1501C57C).
//
// The game's save holds progress and the checkpoint to resume from (scene D_800BE3DF, entrance
// D_800BE3E0), not where Conker stands. A save made from the pause menu (SAVE, or QUIT then Y)
// also records the scene, entrance, position, Conker's facing and the camera's yaw in a file next
// to the save, conker_us.resume (JSON, see parse_points), tagged with the save's sequence number. Starting that save (PLAY on
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
// scene as whole tables of the game's own records (characters, scene objects, triggers, event
// progress; see kTables) and the running event scripts' variables, and puts them back as Conker is
// placed.
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
#include <atomic>
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
#include "enhancements/switches.hpp"

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

// The scene's state is kept as whole tables of the game's own records, restored as they were saved
// except for the words that hold addresses (see restore_tables), so nothing in them needs to be
// understood one field at a time. A table is where its records are, how many there are and how big
// one is:
//   actors        the 25 characters at D_800CC2D0, 0x32C bytes each (Conker first, then the rest)
//   objects       D_800DBEF0 scene objects of 0xA0 bytes at D_800DBEF4 (a gate, a door, a spinning
//                 pickup; byte 0x73 is the state the object moves to: the cheese farm's gate in
//                 scene 12 is 4 shut and 7 open)
//   triggers      D_800D3094 zones of 0x34 bytes at D_800D3098 that start a cutscene or an event
//                 when Conker enters them (func_150A11C4); byte 0x14 marks a one-shot trigger as
//                 fired (Jack's conversation), and func_150A1DA0 skips it then
//   event_status  D_800D2E70, a byte per event (1 running, 3 done)
//   event_flags2  D_800D2E50, 16 bytes of event bits the save leaves out
struct TableSpec {
    const char* name;
    uint32_t address;        // the records, or the pointer to them when `indirect`
    bool indirect;
    uint32_t count_address;  // a 32-bit count, or 0 for `fixed_count` records
    int fixed_count;
    uint32_t record_size;
    int max_count;
    int model_byte;          // actors: a slot is only restored if it holds the same model, or -1
};
constexpr TableSpec kTables[] = {
    {"actors", 0x800CC2D0, false, 0, 25, 0x32C, 25, 0x4},
    {"objects", 0x800DBEF4, true, 0x800DBEF0, 0, 0xA0, 512, -1},
    {"triggers", 0x800D3098, true, 0x800D3094, 0, 0x34, 1024, -1},
    {"event_status", 0x800D2E70, false, 0, 1, 0xCC, 1, -1},
    {"event_flags2", 0x800D2E50, false, 0, 1, 0x10, 1, -1},
};
// The running event scripts: a list {u16 count, head, tail} of nodes {u16 key (event id in the low
// 10 bits), ..., u16 +6 variables' size, u16 +0xA their offset in the node, ..., next +0x18}. Their
// variables are kept per event, as the nodes are allocated anywhere.
constexpr uint32_t kScriptCount = 0x800D2F48;
constexpr uint32_t kScriptHead = 0x800D2F4C;
constexpr int kScriptVariablesSize = 0x6;
constexpr int kScriptVariablesOffset = 0xA;
constexpr int kScriptNext = 0x18;
constexpr uint16_t kScriptEvent = 0x3FF;
constexpr int kMaxScripts = 64;
constexpr size_t kMaxScriptVariables = 1024;
constexpr const char* kPointsFormat = "conker-recomp-resume";
constexpr int kPointsVersion = 2;
constexpr double kMaxCoordinate = 1.0e6;   // well past any level's size
constexpr int kMaxScene = 255;

struct TableState {
    const TableSpec* spec = nullptr;
    int count = 0;
    std::vector<uint8_t> bytes;  // count * record_size, as the game holds them (big-endian words)
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
    std::vector<TableState> tables;      // the scene as it was (see the top of the file)
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

std::atomic<bool> g_snapshots_enabled{[] {
    const char* value = std::getenv("CONKER_SAVE_SNAPSHOTS");
    return value == nullptr || value[0] != '0';
}()};  // the debug menu switches it

bool snapshots_enabled() { return g_snapshots_enabled.load(std::memory_order_relaxed); }

std::filesystem::path points_path() {
    std::filesystem::path path = ultramodern::get_save_file_path();
    path.replace_extension(".resume");
    return path;
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

// A whole number in [low, high] at `key` of `object`.
bool whole_in(const nlohmann::json& object, const char* key, double low, double high, double* out) {
    return number_in(object, key, low, high, out) && *out == std::floor(*out);
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

// `digits` as exactly `size` bytes into `out`; false if it is anything else.
bool from_hex(const std::string& digits, size_t size, std::vector<uint8_t>* out) {
    if (digits.size() != 2 * size) {
        return false;
    }
    out->clear();
    out->reserve(size);
    for (size_t i = 0; i < digits.size(); i += 2) {
        const int high = hex_digit(digits[i]), low = hex_digit(digits[i + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        out->push_back(static_cast<uint8_t>(high * 16 + low));
    }
    return true;
}

// One point of the points file (see parse_points) into `point` and its slot into `slot`.
bool parse_point(const nlohmann::json& entry, Point* point, int* slot) {
    double value, counter, scene, entrance;
    if (!whole_in(entry, "slot", 0, kSlots - 1, &value) || !whole_in(entry, "counter", 0, 4294967295.0, &counter) ||
        !whole_in(entry, "scene", 0, kMaxScene, &scene) || !whole_in(entry, "entrance", 0, 255, &entrance)) {
        return false;
    }
    *slot = static_cast<int>(value);
    point->counter = static_cast<uint32_t>(counter);
    point->scene = static_cast<int>(scene);
    point->entrance = static_cast<int>(entrance);
    float* const coordinates[] = {&point->x, &point->y, &point->z, &point->yaw};
    const char* const names[] = {"x", "y", "z", "yaw"};
    for (int i = 0; i < 4; i++) {
        if (!number_in(entry, names[i], -kMaxCoordinate, kMaxCoordinate, &value)) {
            return false;
        }
        *coordinates[i] = static_cast<float>(value);
    }
    point->has_view = entry.contains("facing") || entry.contains("camera_yaw");
    if (point->has_view) {
        double facing, camera_yaw;
        if (!number_in(entry, "facing", -kMaxCoordinate, kMaxCoordinate, &facing) ||
            !number_in(entry, "camera_yaw", -kMaxCoordinate, kMaxCoordinate, &camera_yaw)) {
            return false;
        }
        point->facing = static_cast<float>(facing);
        point->camera_yaw = static_cast<float>(camera_yaw);
    }
    if (entry.contains("tables")) {
        const nlohmann::json& tables = entry["tables"];
        if (!tables.is_array() || tables.size() > std::size(kTables)) {
            return false;
        }
        for (const nlohmann::json& item : tables) {
            if (!item.is_object() || !item.contains("name") || !item["name"].is_string() || !item.contains("bytes") ||
                !item["bytes"].is_string()) {
                return false;
            }
            const TableSpec* spec = nullptr;
            for (const TableSpec& candidate : kTables) {
                if (item["name"] == candidate.name) {
                    spec = &candidate;
                }
            }
            double count, record_size;
            if (spec == nullptr || !whole_in(item, "count", 1, spec->max_count, &count) ||
                !whole_in(item, "record_size", spec->record_size, spec->record_size, &record_size)) {
                return false;
            }
            for (const TableState& seen : point->tables) {
                if (seen.spec == spec) {
                    return false;
                }
            }
            TableState table;
            table.spec = spec;
            table.count = static_cast<int>(count);
            if (!from_hex(item["bytes"].get_ref<const std::string&>(), static_cast<size_t>(table.count) * spec->record_size,
                          &table.bytes)) {
                return false;
            }
            point->tables.push_back(std::move(table));
        }
    }
    if (entry.contains("scripts")) {
        const nlohmann::json& scripts = entry["scripts"];
        if (!scripts.is_array() || scripts.size() > static_cast<size_t>(kMaxScripts)) {
            return false;
        }
        for (const nlohmann::json& item : scripts) {
            double key;
            if (!whole_in(item, "key", 0, 0xFFFF, &key) || !item.contains("variables") || !item["variables"].is_string()) {
                return false;
            }
            const std::string& digits = item["variables"].get_ref<const std::string&>();
            ScriptVariables script;
            script.key = static_cast<uint16_t>(key);
            if (digits.empty() || digits.size() > 2 * kMaxScriptVariables ||
                !from_hex(digits, digits.size() / 2, &script.bytes)) {
                return false;
            }
            point->scripts.push_back(std::move(script));
        }
    }
    point->valid = true;
    return true;
}

// The points file, conker_us.resume (a snapshot's .resume, a report's save.resume), is JSON:
// {"format": "conker-recomp-resume", "version": 2, "slots": [point, ...]}, at most one point per slot,
// the saved slot's first. A point: "slot", "counter" (the save's sequence number), "scene", "entrance",
// "x", "y", "z", "yaw" (degrees), "facing" and "camera_yaw" when they were recorded, and the scene as
// it was: "tables" [{"name", "count", "record_size", "bytes", hex}] (see kTables) and "scripts"
// [{"key", "variables", hex}]. Everything is checked (a report's may come from anyone); false with
// the reason in `problem`. `first_slot` is the first point's slot, or -1 when there is none.
bool parse_points(const std::string& text, std::array<Point, kSlots>* points, int* first_slot, std::string* problem) {
    *points = {};
    *first_slot = -1;
    const nlohmann::json doc = nlohmann::json::parse(text, nullptr, false);
    if (!doc.is_object() || !doc.contains("format") || doc["format"] != kPointsFormat || !doc.contains("version") ||
        doc["version"] != kPointsVersion || !doc.contains("slots") || !doc["slots"].is_array() ||
        doc["slots"].size() > static_cast<size_t>(kSlots)) {
        *problem = "not a version 2 resume file";
        return false;
    }
    for (const nlohmann::json& entry : doc["slots"]) {
        Point point;
        int slot;
        if (!parse_point(entry, &point, &slot) || (*points)[slot].valid) {
            *problem = "a point is not valid";
            *points = {};
            *first_slot = -1;
            return false;
        }
        (*points)[slot] = std::move(point);
        if (*first_slot < 0) {
            *first_slot = slot;
        }
    }
    return true;
}

std::array<Point, kSlots> read_points(const std::filesystem::path& path, int* first_slot = nullptr) {
    std::array<Point, kSlots> points{};
    int first;
    std::ifstream file(path);
    if (file) {
        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        std::string problem;
        if (!parse_points(text, &points, &first, &problem)) {
            std::printf("[conker] %s: %s; no resume points\n", path.string().c_str(), problem.c_str());
        }
    }
    if (first_slot != nullptr) {
        *first_slot = file ? first : -1;
    }
    return points;
}

std::array<Point, kSlots> read_points() { return read_points(points_path()); }

std::string to_hex(const std::vector<uint8_t>& bytes) {
    std::string text;
    for (uint8_t byte : bytes) {
        char digits[3];
        std::snprintf(digits, sizeof(digits), "%02x", byte);
        text += digits;
    }
    return text;
}

// `first_slot`'s point goes first, so a snapshot says which slot it saved.
void write_points(const std::array<Point, kSlots>& points, const std::filesystem::path& path, int first_slot = -1) {
    nlohmann::json slots = nlohmann::json::array();
    auto add = [&](int slot) {
        const Point& p = points[slot];
        if (!p.valid) {
            return;
        }
        nlohmann::json entry = {{"slot", slot}, {"counter", p.counter}, {"scene", p.scene}, {"entrance", p.entrance},
                                {"x", p.x},     {"y", p.y},             {"z", p.z},         {"yaw", p.yaw}};
        if (p.has_view) {
            entry["facing"] = p.facing;
            entry["camera_yaw"] = p.camera_yaw;
        }
        nlohmann::json tables = nlohmann::json::array();
        for (const TableState& table : p.tables) {
            tables.push_back({{"name", table.spec->name},
                              {"count", table.count},
                              {"record_size", table.spec->record_size},
                              {"bytes", to_hex(table.bytes)}});
        }
        nlohmann::json scripts = nlohmann::json::array();
        for (const ScriptVariables& script : p.scripts) {
            scripts.push_back({{"key", script.key}, {"variables", to_hex(script.bytes)}});
        }
        entry["tables"] = std::move(tables);
        entry["scripts"] = std::move(scripts);
        slots.push_back(std::move(entry));
    };
    if (first_slot >= 0 && first_slot < kSlots) {
        add(first_slot);
    }
    for (int slot = 0; slot < kSlots; slot++) {
        if (slot != first_slot) {
            add(slot);
        }
    }
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::trunc) << nlohmann::json{{"format", kPointsFormat}, {"version", kPointsVersion},
                                                           {"slots", std::move(slots)}}
                                                .dump(1)
                                         << '\n';
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

// Where `spec`'s records are and how many there are now; 0 when there are none (no scene).
uint32_t table_records(uint8_t* rdram, const TableSpec& spec, int* count) {
    const uint32_t records = spec.indirect ? static_cast<uint32_t>(MEM_W(0, kseg0(spec.address))) : spec.address;
    *count = spec.count_address != 0 ? MEM_W(0, kseg0(spec.count_address)) : spec.fixed_count;
    if (!in_rdram(records) || *count <= 0 || *count > spec.max_count ||
        !in_rdram(records + static_cast<uint32_t>(*count) * spec.record_size - 1)) {
        *count = 0;
        return 0;
    }
    return records;
}

// A word that holds an address, in RDRAM or in the game's code: it belongs to this run of the game
// (where things were allocated), so it is never restored.
bool is_address(uint32_t word) {
    const uint32_t top = word >> 24;
    return (top >= 0x80 && top < 0x88) || top == 0x10 || top == 0x15 || top == 0x16;
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
    point->tables.clear();
    point->scripts.clear();
    for (const TableSpec& spec : kTables) {
        TableState table;
        const uint32_t records = table_records(rdram, spec, &table.count);
        if (table.count == 0) {
            continue;
        }
        table.spec = &spec;
        const uint32_t size = static_cast<uint32_t>(table.count) * spec.record_size;
        for (uint32_t i = 0; i < size; i++) {
            table.bytes.push_back(MEM_BU(static_cast<int>(i), kseg0(records)));
        }
        point->tables.push_back(std::move(table));
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

// Puts back the scene `point` recorded: each table whose record count is the same as now (an actor
// slot only if it holds the same model), word by word, except the words that hold an address now or
// did then (is_address), and the variables of each event script running again (the same event, the
// same size). The game then carries on from there: objects move to the states they had, a fired
// trigger stays fired.
void restore_scene(uint8_t* rdram, const Point& point) {
    std::string restored;
    for (const TableState& table : point.tables) {
        const TableSpec& spec = *table.spec;
        int count;
        const uint32_t records = table_records(rdram, spec, &count);
        if (count != table.count) {
            restored += std::string(" ") + spec.name + " (skipped, " + std::to_string(count) + " now, " +
                        std::to_string(table.count) + " saved)";
            continue;
        }
        int records_restored = 0;
        for (int index = 0; index < count; index++) {
            const uint32_t record = records + static_cast<uint32_t>(index) * spec.record_size;
            const uint8_t* saved = table.bytes.data() + static_cast<size_t>(index) * spec.record_size;
            if (spec.model_byte >= 0 && MEM_BU(spec.model_byte, kseg0(record)) != saved[spec.model_byte]) {
                continue;
            }
            for (uint32_t offset = 0; offset < spec.record_size; offset += 4) {
                const uint32_t then = (uint32_t{saved[offset]} << 24) | (uint32_t{saved[offset + 1]} << 16) |
                                      (uint32_t{saved[offset + 2]} << 8) | saved[offset + 3];
                const uint32_t now = static_cast<uint32_t>(MEM_W(0, kseg0(record + offset)));
                if (!is_address(then) && !is_address(now)) {
                    MEM_W(0, kseg0(record + offset)) = static_cast<int32_t>(then);
                }
            }
            records_restored++;
        }
        restored += std::string(" ") + spec.name + " " + std::to_string(records_restored) + "/" + std::to_string(count);
    }
    int scripts = 0;
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
    std::printf("[conker] resumed scene state:%s, event scripts %d/%zu\n", restored.c_str(), scripts,
                point.scripts.size());
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

    // save.resume: a valid points file (parse_points) starting with the report's slot, whose point
    // is the report's.
    std::array<Point, kSlots> points;
    int first_slot;
    std::string points_problem;
    if (!parse_points(files->points, &points, &first_slot, &points_problem)) {
        *problem = "save.resume: " + points_problem;
        return false;
    }
    if (first_slot != static_cast<int>(slot)) {
        *problem = "save.resume does not start with the report's slot";  // the slot a load starts
        return false;
    }
    const Point& point = points[first_slot];
    const bool found = point.counter == static_cast<uint32_t>(counter) && point.scene == static_cast<int>(scene) &&
                       std::fabs(point.x - x) < 0.01 && std::fabs(point.y - y) < 0.01 && std::fabs(point.z - z) < 0.01;
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
    if (g_resume_frames == kSettleFrames + 1 && (!g_resuming.tables.empty() || !g_resuming.scripts.empty())) {
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

conker::switches::Switch conker::switches::save_snapshots() {
    return {"SAVE SNAPSHOTS", conker::resume::snapshots_enabled, [](bool on) { conker::resume::g_snapshots_enabled.store(on); }};
}
