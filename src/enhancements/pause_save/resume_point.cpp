// Exact-position saves. Native helpers called from the [[patches.hook]] snippets in config/us.toml
// (func_15006BEC, func_151E327C, func_1501C57C).
//
// The game's save holds progress and the checkpoint to resume from (scene D_800BE3DF, entrance
// D_800BE3E0), not where Conker stands. A save made from the pause menu (SAVE, or QUIT then Y)
// also records the scene, entrance, position, Conker's facing and the camera's yaw, distance and
// pitch in a file next to the save, conker_us.resume (JSON, see parse_points), tagged with the save's sequence number. Starting that save (PLAY on
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
#include "enhancements/frame_rate/frame_rate.hpp"
#include "enhancements/status.hpp"
#include "enhancements/switches.hpp"

extern "C" void func_151E327C(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_15178DA4(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_15082A44(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_15060F28(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_1501D348(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_1509B5AC(uint8_t* rdram, recomp_context* ctx);
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
// The same block (the player's, see func_150BDB70) holds what B does where Conker stands: u8 +0x75,
// and a second action at +0x74. A B pad's trigger sets them as he steps on it (func_15080500,
// which leaves one alone while it has 0x80), and a script sets one with 0x80 to make it stick away
// from any pad (func_15080620): drinking at Rock Solid's B pad leaves 0x83 at +0x75, which is him
// drunk: B pees. How long he stays drunk is the s16 at +0x8, counting down a step a frame (about a
// minute from the drink); while it is above 0 he hiccups and staggers instead of standing still
// (func_1507F640 picks animation 0xAB). The block is not in the saved tables, so a point keeps the
// sticky actions (conker_actions) and what is left of that count (conker_drunk) and puts them back.
constexpr int kPlayerActions[] = {0x74, 0x75};
constexpr uint8_t kActionSticky = 0x80;
constexpr int kPlayerDrunk = 0x8;
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
// How far along the ground the camera stays from Conker: it eases towards a target distance
// (+0x374; the controls' +0x370 is the distance it is at), which the scene's camera zones set (an
// entrance's zone sets its own, and leaving one puts back the player's distance, func_15124B18), as
// does a quick zoom (func_1512B730): it keeps the target it replaced in +0x3F4 (-1 when none) for
// 30 ticks (+0x3F0), then puts it back.
constexpr int kTargetDistance = 0x374;
constexpr int kZoomKeptDistance = 0x3F4;
constexpr uint32_t kScene = 0x800BE9F0;          // D_800BE9F0: the scene being played
// D_800BE3DF, D_800BE3E0: the checkpoint the save restarts from (scene, entrance), set by the event
// scripts (func_1509F850) and read back from the save (func_15006590). A scene started as the game
// is started (func_15007B3C, when func_1509C2A4 says so) takes the entrance it is entered by as the
// checkpoint's: right for PLAY, which enters the checkpoint scene by the checkpoint entrance, but a
// resumed save enters its point's scene by the point's entrance, so the checkpoint entrance was
// overwritten with that, and the next save carried it (a death or a QUIT then restarted there). So
// the checkpoint is noted as the save holds it and put back once the scene is running.
constexpr uint32_t kCheckpointScene = 0x800BE3DF;
constexpr uint32_t kCheckpointEntrance = 0x800BE3E0;
constexpr uint32_t kEntrance = 0x800DD2C0;       // D_800DD2C0: the entrance it was entered by
constexpr int kObjectPreviousX = 0x2C;           // last frame's position
constexpr uint32_t kTransition = 0x800BE615;     // D_800BE615: scene change pending, 0 when none
constexpr int kSettleFrames = 10;                // frames the scene runs before Conker is moved
constexpr int kPlaceFrames = 4;                  // frames he is then held at the resumed spot
constexpr int kTeleportFrames = 8;               // frames he is held after a teleport (debug menu)
constexpr uint32_t kSaveCounters = 0x800BE400;   // D_800BE3F8 + 8, 16 bytes per slot
constexpr int kSlotRecord = 6;                   // D_800BE3F8 + 0xE: the record holding the slot
constexpr uint32_t kRecordSlots = 0x800BE3D8;    // D_800BE3D8: the slot each record holds, or -1
constexpr int kRecords = 4;                      // save records (see note_records)
constexpr uint32_t kFirstRecordBlock = 4;
constexpr uint32_t kRecordBlocks = 16;
constexpr uint32_t kRecordSize = 0x80;
constexpr uint32_t kRecordChecksumStart = 0xCC;
constexpr uint32_t kSaveSlot = 0x8008FDA8;       // D_8008FDA8: the game slot in play
constexpr uint32_t kEepromAction = 0x80082BB4;   // D_80082BB4: request for func_1501C57C
constexpr uint32_t kEepromSlot = 0x800BE3EC;     // D_800BE3EC: its argument (a slot to save, a record to load)
constexpr int8_t kEepromLoad = 1;
constexpr int8_t kEepromSave = 2;
constexpr uint32_t kChapterReplay = 0x800BE616;  // D_800BE616: playing from CHAPTERS, saves off
constexpr std::chrono::seconds kReportSaveWait{5};  // a report's save not written by then: none
constexpr uint32_t kEepromWriting = 0x800E0A20;  // D_800E0A20: paced EEPROM write under way, 0 when none
constexpr uint32_t kEepromSize = 0x800;          // 16 kbit
constexpr uint32_t kSnapshotScratch = 0x807F9000;  // free RDRAM, see pause_save.cpp
constexpr uint32_t kReportBlockScratch = 0x807F9800;  // 8 bytes after it
constexpr int kSlots = 3;
constexpr uint32_t kSlotSave = 0x20;             // a slot's save (D_800BE358) is at block 4 + slot * 16,
constexpr uint32_t kSlotSaveSize = 0x80;         // 0x80 bytes (func_15006BEC): in it
constexpr int kSavedSlot = 2;                    // the slot,
constexpr int kSavedEvents = 7;                  // and the event flags, *D_800D2E4C's 0x1B bytes
constexpr int kEventBytes = 0x1B;
// A save the game makes this soon after resuming (it saves on entering some areas) is still the
// resumed one: the point carries over to it.
constexpr std::chrono::seconds kResumeSaveWindow{10};

// The scene's state is kept as whole tables of the game's own records, restored as they were saved
// except for the words that hold addresses (see restorable) and a character's link to another
// (restored_link), so nothing else in them needs to be understood one field at a time. A table is
// where its records are, how many there are and how big one is:
//   actors        the 25 characters at D_800CC2D0, 0x32C bytes each (Conker first, then the rest)
//   objects       D_800DBEF0 scene objects of 0xA0 bytes at D_800DBEF4 (a gate, a door, a spinning
//                 pickup; byte 0x73 is the state the object moves to: the cheese farm's gate in
//                 scene 12 is 4 shut and 7 open)
//   triggers      D_800D3094 zones of 0x34 bytes at D_800D3098 that start a cutscene or an event
//                 when Conker enters them (func_150A11C4); byte 0x14 marks a one-shot trigger as
//                 fired (Jack's conversation), and func_150A1DA0 skips it then
//   event_status  D_800D2E70, a byte per event (1 running, 3 done)
//   event_flags2  D_800D2E50, 16 bytes of event bits the save leaves out
//   players       the 4 players' records of 0x1C bytes at D_800D213C (func_15085710 sets their
//                 fields, func_150859AC reads them): byte 8 is the lives (D_800D2144 for Conker),
//                 which the game puts back to its starting count when a save is loaded
struct TableSpec {
    const char* name;
    uint32_t address;        // the records, or the pointer to them when `indirect`
    bool indirect;
    uint32_t count_address;  // a 32-bit count, or 0 for `fixed_count` records
    int fixed_count;
    uint32_t record_size;
    int max_count;
    int model_byte;          // actors: a slot is only restored if it holds the same model, or -1
    int id_offset = -1;      // actors: a u16 naming the character in the scene, or -1 (see match_records)
    bool by_place = false;   // objects: records are paired by index and position (see match_objects)
};
constexpr TableSpec kTables[] = {
    {"actors", 0x800CC2D0, false, 0, 25, 0x32C, 25, 0x4, 0x3A},
    {"objects", 0x800DBEF4, true, 0x800DBEF0, 0, 0xA0, 512, -1, -1, true},
    {"triggers", 0x800D3098, true, 0x800D3094, 0, 0x34, 1024, -1},
    {"event_status", 0x800D2E70, false, 0, 1, 0xCC, 1, -1},
    {"event_flags2", 0x800D2E50, false, 0, 1, 0x10, 1, -1},
    {"players", 0x800D213C, false, 0, 4, 0x1C, 4, -1},
};
// A scene object (kTables, "objects"): its own index, u8 +0x71, the one stable name it has (its
// flags, u8 +0x4F, change as it is shown and hidden, and its position as it moves).
constexpr int kObjectIndex = 0x71;
constexpr uint32_t kActors = 0x800CC2D0;
constexpr uint32_t kActorSize = 0x32C;
constexpr int kActorCount = 25;
constexpr int kActorPosition = 0x14;  // floats
constexpr int kActorPlacement = 0x13F;  // u8, the scene placement it was made from, 0xFF for none
// u8, the slot + 1 of the character this one keeps to, 0 for none: a child the game makes for a
// character (func_15083AC8) records its parent there, and in Count Batula's mansion (scene 60) the
// bat that circles over the grinder keeps to Batula's perch. A slot number, so it changes with the
// slots (see restored_link).
constexpr int kActorLink = 0x65;
// The scene's character placements: *D_800D20FC records of 0x30 bytes (+4 the model, +2 cleared by
// a script before it makes one), D_800D2100 of them, and *D_800D210C a byte per placement, the slot
// its character is in + 1 (0 while there is none; 0x80 is a flag of its own). func_15082A44(record,
// placement, 0, 0, slot 0) makes a placement's character in a free slot, as the event scripts do
// (0x15097980); func_15060F28(actor, 0) takes one away and frees its placement.
// A cutscene (func_1501D348(scene, script, 0, 0, 0) starts one, as a trigger or an event script
// does): D_800C35EA is 1 while one plays, D_800C35E8 its script in the scene.
constexpr uint32_t kCutscenePlaying = 0x800C35EA;
constexpr uint32_t kCutsceneScript = 0x800C35E8;
constexpr uint32_t kPlacements = 0x800D20FC;
constexpr uint32_t kPlacementCount = 0x800D2100;
constexpr uint32_t kPlacementSlots = 0x800D210C;
constexpr uint32_t kPlacementSize = 0x30;
constexpr int kPlacementModel = 0x4;
constexpr int kPlacementCleared = 0x2;
// Swarms of bees, as Conker leads them to the sunflower in scene 12 (event 55): effect objects of
// type 58 (see func_151670C0, which runs each type's list), in a list at D_800DCF38 of nodes
// {..., next +0x8, ..., target +0x10, position +0x18 (three s32), the position last frame +0x28
// (three s16), ..., +0x30 a speed, id +0x34}. The event's script points a swarm by its id
// (func_15178B98) at a spot, three s16 at +0x10 (func_15178C34), or at a character's position,
// its address at +0x10 and 0x8000 in the high half of +0x14 (func_15178BE4): Conker's while it
// follows him, the sunflower's once it is delivered. The game makes them again from the start of
// the event when a save is loaded, all at home; the point keeps them as they were (see
// restore_swarms). The bees of type 58's list D_800DCF3C fly around their swarm (+0x14), at s16
// +0x20, and are drawn only while it is in view (func_151787AC).
constexpr uint32_t kSwarms = 0x800DCF38;
constexpr uint32_t kBees = 0x800DCF3C;
constexpr int kEffectNext = 0x8;
constexpr int kSwarmTarget = 0x10;
constexpr int kSwarmTargetFlag = 0x14;  // s16, -0x8000 when +0x10 holds an address
constexpr int kSwarmPosition = 0x18;
constexpr int kSwarmLastPosition = 0x28;
constexpr int kSwarmSpeed = 0x30;
constexpr int kSwarmId = 0x34;
constexpr int kBeeSwarm = 0x14;
constexpr int kBeePosition = 0x20;
constexpr int kMaxSwarms = 32;
constexpr int kMaxBees = 1024;
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
// A camera nearer than this to the point it looks at is a first-person one (on a mounted gun it
// sits on that point, about 1 away). A point keeps the distance as recorded, but a resumed save
// lets the game place its camera instead: the restored scene puts Conker back on the gun, whose
// own camera takes over, and making that distance the target one (keep_distance) would leave the
// third-person camera inside his head once he gets off.
constexpr float kMinCameraDistance = 50.0f;
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

struct Swarm {
    int id = 0;
    int32_t position[3] = {};
    int target_actor = -1;   // the character it flies after, or -1 for `target`
    int16_t target[3] = {};
    int16_t speed = 0;
};

struct Point {
    bool valid = false;
    uint32_t counter = 0;  // the save's sequence number
    int scene = 0;
    int entrance = 0;
    float x = 0, y = 0, z = 0, yaw = 0;  // yaw in degrees
    bool has_view = false;               // facing and camera_yaw recorded (see Position)
    float facing = 0, camera_yaw = 0;
    bool has_camera = false;             // camera_distance and camera_pitch recorded (an older
    float camera_distance = 0;           // point has none): from the camera to the point it looks
    float camera_pitch = 0;              // at, and its pitch in degrees (see measure_camera)
    std::vector<TableState> tables;      // the scene as it was (see the top of the file)
    std::vector<ScriptVariables> scripts;
    bool has_swarms = false;             // recorded (an older point has none)
    std::vector<Swarm> swarms;
    int cutscene = -1;                   // the scene's cutscene to play again once placed, or -1
    uint8_t actions[2] = {};             // Conker's sticky B actions (kPlayerActions), 0 for none
    int drunk = 0;                       // frames he has left drunk (kPlayerDrunk)
};

Point g_pending;           // remembered, waiting for the save to be written
// The scene as the running cutscene found it (see note_cutscene), for a save made during it.
Point g_cutscene_start;
bool g_cutscene_was_playing = false;
int g_pending_slot = -1;
bool g_load_requested = false;
Point g_resuming;          // being started, waiting for Conker to be placed
int g_resume_frames = 0;
int g_place_frames = kPlaceFrames;
Position g_view;           // a teleport's facing and camera, held with its position
float g_camera_distance = 600.0f;  // from the camera to the point it looks at, and its pitch,
float g_camera_pitch = -30.0f;     // when a teleport was asked for or a resumed save first placed
// A resumed save's camera is put back once it has settled behind Conker (it swings into place as
// the scene starts), then held there for a few frames. It goes back as far from Conker and at the
// pitch it was saved with when the point recorded them, and the game's target distance is set to
// match (see kTargetDistance). Where the entrance camera comes to rest is otherwise a race in the
// game: in scene 0's report (saved 530 along the ground from Conker) the entrance zone ends during a
// quick zoom, and at 30 fps the zoom runs out last, putting back the zone's 211, so the camera
// eased back in after the hold.
bool g_camera_waits = false;
bool g_camera_saved = false;       // g_camera_distance and g_camera_pitch are the point's
int g_camera_still = 0;            // frames it has not moved
int g_camera_wait = 0;             // frames waited
int g_camera_hold = 0;             // frames left to hold it
float g_last_eye[3] = {};
constexpr int kCameraStillFrames = 10;
constexpr int kCameraWaitFrames = 150;  // about 5 s
bool g_at_checkpoint = false;  // start the next save at its checkpoint, not its point (load_report)
bool g_on_report_copy = false;  // playing on a debug report's copy of the save (load_report)
bool g_keep_pending = false;    // a pause save on that copy to put in the player's save once written
int g_keep_slot = 0;
Point g_keep_point;
int g_checkpoint[2] = {-1, -1};  // the resumed save's checkpoint (scene, entrance), to put back; -1 none
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
// A save the debug menu starts (load_snapshot, load_report) is started behind a black screen: the
// game's viewport fade, held black (conker_skip_fade_alpha in intro_skip.cpp draws it), from the
// press until the scene it goes to is running with Conker placed and its circle wipe opens on him
// (update_cover). Otherwise the wipe's small circle shows the scene's entrance, and Conker jumping
// to the point, before it opens. The fade is not drawn while the pause closes, so the game is
// only started once it has been drawn for a few frames: the game never clears its colour buffers,
// and while the scene changes the old scene would show at the widescreen sides of the closed
// circle wipe, a different frame in each of the two buffers.
enum class Cover { None, Black, FadingIn };
Cover g_cover = Cover::None;
bool g_cover_drawn = false;    // drawn black since the last frame (load_cover)
int g_cover_black = 0;         // frames it has been drawn black
bool g_cover_started = false;  // the game was started behind it (func_151E327C)
bool g_cover_changed = false;  // and its scene change has begun
int g_cover_frames = 0;        // frames since, with everything in place
std::chrono::steady_clock::time_point g_cover_since;
// The circle wipe that opens a scene, for viewport 0: how closed it is (D_800DDDC8, 1 closed to 0
// open), and its change per tick (D_800DDDD8): -0.004 as the scene starts, 0 while it holds a small
// circle around Conker, -0.009 as it opens.
constexpr uint32_t kWipe = 0x800DDDC8;
constexpr uint32_t kWipeSpeed = 0x800DDDD8;
constexpr float kWipeOpening = -0.006f;
constexpr int kCoverBlackFrames = 3;               // frames drawn black before the game is started
constexpr int kCoverFrames = 4;                    // frames everything stays in place before the fade
constexpr std::chrono::milliseconds kCoverFade{330};  // as the intro skip's
constexpr std::chrono::seconds kCoverLimit{5};     // shown by then whatever happens
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
    point->has_camera = entry.contains("camera_distance") || entry.contains("camera_pitch");
    if (point->has_camera) {
        double distance, pitch;
        if (!point->has_view || !number_in(entry, "camera_distance", 0, kMaxCoordinate, &distance) ||
            !number_in(entry, "camera_pitch", -90, 90, &pitch)) {
            return false;
        }
        point->camera_distance = static_cast<float>(distance);
        point->camera_pitch = static_cast<float>(pitch);
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
    if (entry.contains("swarms")) {
        const nlohmann::json& swarms = entry["swarms"];
        if (!swarms.is_array() || swarms.size() > static_cast<size_t>(kMaxSwarms)) {
            return false;
        }
        for (const nlohmann::json& item : swarms) {
            Swarm swarm;
            double id, speed;
            if (!whole_in(item, "id", 0, 255, &id) || !whole_in(item, "speed", -32768, 32767, &speed) ||
                !item.contains("position") || !item["position"].is_array() || item["position"].size() != 3) {
                return false;
            }
            swarm.id = static_cast<int>(id);
            swarm.speed = static_cast<int16_t>(speed);
            for (int axis = 0; axis < 3; axis++) {
                const nlohmann::json& value = item["position"][axis];
                if (!value.is_number_integer() || value.get<double>() < -kMaxCoordinate ||
                    value.get<double>() > kMaxCoordinate) {
                    return false;
                }
                swarm.position[axis] = value.get<int32_t>();
            }
            double actor;
            if (item.contains("target_actor")) {
                if (!whole_in(item, "target_actor", 0, kActorCount - 1, &actor)) {
                    return false;
                }
                swarm.target_actor = static_cast<int>(actor);
            } else {
                if (!item.contains("target") || !item["target"].is_array() || item["target"].size() != 3) {
                    return false;
                }
                for (int axis = 0; axis < 3; axis++) {
                    const nlohmann::json& value = item["target"][axis];
                    if (!value.is_number_integer() || value.get<double>() < -32768 || value.get<double>() > 32767) {
                        return false;
                    }
                    swarm.target[axis] = static_cast<int16_t>(value.get<int32_t>());
                }
            }
            for (const Swarm& seen : point->swarms) {
                if (seen.id == swarm.id) {
                    return false;
                }
            }
            point->swarms.push_back(swarm);
        }
        point->has_swarms = true;
    }
    if (entry.contains("conker_actions")) {
        const nlohmann::json& actions = entry["conker_actions"];
        if (!actions.is_array() || actions.size() != std::size(kPlayerActions)) {
            return false;
        }
        for (size_t i = 0; i < actions.size(); i++) {
            const nlohmann::json& action = actions[i];
            if (!action.is_number_integer() || action.get<int>() < 0 || action.get<int>() > 255 ||
                (action.get<int>() != 0 && !(action.get<int>() & kActionSticky))) {
                return false;
            }
            point->actions[i] = static_cast<uint8_t>(action.get<int>());
        }
    }
    if (entry.contains("conker_drunk")) {
        double drunk;
        if (!whole_in(entry, "conker_drunk", 0, 32767, &drunk)) {
            return false;
        }
        point->drunk = static_cast<int>(drunk);
    }
    if (entry.contains("cutscene")) {
        double cutscene;
        if (!whole_in(entry, "cutscene", 0, 255, &cutscene)) {
            return false;
        }
        point->cutscene = static_cast<int>(cutscene);
    }
    point->valid = true;
    return true;
}

// The points file, conker_us.resume (a snapshot's .resume, a report's save.resume), is JSON:
// {"format": "conker-recomp-resume", "version": 2, "slots": [point, ...]}, at most one point per slot,
// the saved slot's first. A point: "slot", "counter" (the save's sequence number), "scene", "entrance",
// "x", "y", "z", "yaw" (degrees), "facing" and "camera_yaw" when they were recorded, with
// "camera_distance" and "camera_pitch" (degrees) in newer points, and the scene as
// it was: "tables" [{"name", "count", "record_size", "bytes", hex}] (see kTables), "scripts"
// [{"key", "variables", hex}] and "swarms" [{"id", "position" [x, y, z], "target_actor" or "target"
// [x, y, z], "speed"}] (see kSwarms), and "cutscene" when the save was made during one (see
// note_cutscene). Everything is checked (a report's may come from anyone); false with
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
            if (p.has_camera) {
                entry["camera_distance"] = p.camera_distance;
                entry["camera_pitch"] = p.camera_pitch;
            }
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
        if (p.has_swarms) {
            nlohmann::json swarms = nlohmann::json::array();
            for (const Swarm& swarm : p.swarms) {
                nlohmann::json item = {{"id", swarm.id},
                                       {"position", {swarm.position[0], swarm.position[1], swarm.position[2]}},
                                       {"speed", swarm.speed}};
                if (swarm.target_actor >= 0) {
                    item["target_actor"] = swarm.target_actor;
                } else {
                    item["target"] = {swarm.target[0], swarm.target[1], swarm.target[2]};
                }
                swarms.push_back(std::move(item));
            }
            entry["swarms"] = std::move(swarms);
        }
        if (p.cutscene >= 0) {
            entry["cutscene"] = p.cutscene;
        }
        if (p.actions[0] != 0 || p.actions[1] != 0) {
            entry["conker_actions"] = {p.actions[0], p.actions[1]};
        }
        if (p.drunk > 0) {
            entry["conker_drunk"] = p.drunk;
        }
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

// A word that holds an address in RDRAM: it belongs to this run of the game (where things were
// allocated). Addresses in the game's code are the same in every run, so they are not among them;
// counting them in too (0x10, 0x15, 0x16 high bytes) once left out a byte field of 0x16, scene 12's
// walking crate's state (actor +0x138), which then went back to where it started.
bool is_address(uint32_t word) { return word >= 0x80000400 && word < 0x80800000; }

// Whether the word saved as `then` goes back over the one there `now`: not an address of the saved
// run, and not an empty field over something the game has since allocated.
bool restorable(uint32_t then, uint32_t now) { return !is_address(then) && !(then == 0 && is_address(now)); }

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

// Calls `visit(node)` for each effect object in the list at `head` (kSwarms, kBees), at most `limit`.
template <typename Visit>
void for_each_effect(uint8_t* rdram, uint32_t head, int limit, Visit visit) {
    uint32_t node = static_cast<uint32_t>(MEM_W(0, kseg0(head)));
    for (int i = 0; i < limit && in_rdram(node); i++) {
        const uint32_t next = static_cast<uint32_t>(MEM_W(0, kseg0(node + kEffectNext)));
        visit(node);
        node = next;
    }
}

// The character whose position `address` is, or -1.
int actor_at(uint32_t address) {
    if (address < kActors + kActorPosition || address >= kActors + kActorCount * kActorSize) {
        return -1;
    }
    const uint32_t offset = address - kActors - kActorPosition;
    return offset % kActorSize == 0 ? static_cast<int>(offset / kActorSize) : -1;
}

// Records the swarms (see kSwarms) in `point`; none if one flies after anything but a character,
// so that restore_swarms leaves them as the game makes them.
void capture_swarms(uint8_t* rdram, Point* point) {
    point->swarms.clear();
    point->has_swarms = true;
    for_each_effect(rdram, kSwarms, kMaxSwarms, [&](uint32_t node) {
        Swarm swarm;
        swarm.id = MEM_BU(kSwarmId, kseg0(node));
        for (int axis = 0; axis < 3; axis++) {
            swarm.position[axis] = MEM_W(kSwarmPosition + 4 * axis, kseg0(node));
            swarm.target[axis] = MEM_H(kSwarmTarget + 2 * axis, kseg0(node));
        }
        swarm.speed = MEM_H(kSwarmSpeed, kseg0(node));
        if (MEM_H(kSwarmTargetFlag, kseg0(node)) == -0x8000) {
            swarm.target_actor = actor_at(static_cast<uint32_t>(MEM_W(kSwarmTarget, kseg0(node))));
            if (swarm.target_actor < 0) {
                point->has_swarms = false;
            }
        }
        point->swarms.push_back(swarm);
    });
    if (!point->has_swarms) {
        point->swarms.clear();
    }
}

// Puts the swarms back as `point` recorded them, each by its id: where it was, what it flew after
// and how fast, with its bees moved along with it (they would otherwise stream across the level
// from the swarm's home). A swarm the point did not have (one already gone then) is taken away
// with its bees (func_15178DA4); one the game has not made again cannot be. The character a swarm
// flies after is the one saved in that slot, in whichever slot it is now (`actor_slots`, see
// match_records; left as the game made it if that character is not here).
void restore_swarms(uint8_t* rdram, recomp_context* ctx, const Point& point, const std::vector<int>& actor_slots,
                    std::string* restored) {
    int moved = 0;
    std::vector<uint32_t> extra;
    for_each_effect(rdram, kSwarms, kMaxSwarms, [&](uint32_t node) {
        const int id = MEM_BU(kSwarmId, kseg0(node));
        const auto saved = std::find_if(point.swarms.begin(), point.swarms.end(), [&](const Swarm& s) { return s.id == id; });
        if (saved == point.swarms.end()) {
            extra.push_back(node);
            return;
        }
        int32_t shift[3];
        for (int axis = 0; axis < 3; axis++) {
            shift[axis] = saved->position[axis] - MEM_W(kSwarmPosition + 4 * axis, kseg0(node));
            MEM_W(kSwarmPosition + 4 * axis, kseg0(node)) = saved->position[axis];
            MEM_H(kSwarmLastPosition + 2 * axis, kseg0(node)) = static_cast<int16_t>(saved->position[axis]);
        }
        if (saved->target_actor >= 0) {
            const int slot = saved->target_actor < static_cast<int>(actor_slots.size()) ? actor_slots[saved->target_actor]
                                                                                       : saved->target_actor;
            if (slot >= 0) {
                MEM_W(kSwarmTarget, kseg0(node)) =
                    static_cast<int32_t>(kActors + static_cast<uint32_t>(slot) * kActorSize + kActorPosition);
                MEM_W(kSwarmTargetFlag, kseg0(node)) = static_cast<int32_t>(0x80000000u);
            }
        } else {
            for (int axis = 0; axis < 3; axis++) {
                MEM_H(kSwarmTarget + 2 * axis, kseg0(node)) = saved->target[axis];
            }
        }
        MEM_H(kSwarmSpeed, kseg0(node)) = saved->speed;
        for_each_effect(rdram, kBees, kMaxBees, [&](uint32_t bee) {
            if (static_cast<uint32_t>(MEM_W(kBeeSwarm, kseg0(bee))) != node) {
                return;
            }
            for (int axis = 0; axis < 3; axis++) {
                const int32_t moved_to = MEM_H(kBeePosition + 2 * axis, kseg0(bee)) + shift[axis];
                MEM_H(kBeePosition + 2 * axis, kseg0(bee)) = static_cast<int16_t>(std::clamp(moved_to, -32768, 32767));
            }
        });
        moved++;
    });
    for (uint32_t node : extra) {
        recomp_context saved = *ctx;
        ctx->r4 = kseg0(node);
        func_15178DA4(rdram, ctx);
        *ctx = saved;
    }
    *restored += " swarms " + std::to_string(moved) + "/" + std::to_string(point.swarms.size());
    if (!extra.empty()) {
        *restored += " (" + std::to_string(extra.size()) + " taken away)";
    }
}

// Conker's player block (kObjectCamera, also the camera's), or 0.
uint32_t player_block(uint8_t* rdram) {
    const uint32_t conker = static_cast<uint32_t>(MEM_W(0, kseg0(kPlayerObject)));
    if (conker < 0x80000000u || conker >= 0x80800000u) {
        return 0;
    }
    const uint32_t player = static_cast<uint32_t>(MEM_W(kObjectCamera, kseg0(conker)));
    return player >= 0x80000000u && player < 0x80800000u ? player : 0;
}

// Records the scene as it is now (see the top of the file) in `point`.
void capture_scene(uint8_t* rdram, Point* point) {
    capture_swarms(rdram, point);
    const uint32_t player = player_block(rdram);
    for (size_t i = 0; i < std::size(kPlayerActions); i++) {
        const uint8_t action = player != 0 ? MEM_BU(kPlayerActions[i], kseg0(player)) : 0;
        point->actions[i] = (action & kActionSticky) ? action : 0;
    }
    point->drunk = player != 0 ? std::max<int>(0, MEM_H(kPlayerDrunk, kseg0(player))) : 0;
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

// Calls one of the game's functions with `a0`, `a1` and zeros for the other arguments (a fifth on
// the stack too), from a hook, leaving the hook's registers as they were.
void call_game(uint8_t* rdram, recomp_context* ctx, void (*function)(uint8_t*, recomp_context*), uint32_t a0,
               uint32_t a1) {
    recomp_context saved = *ctx;
    ctx->r29 -= 0x40;  // room for the fifth argument below the hook's caller
    ctx->r4 = static_cast<gpr>(static_cast<int32_t>(a0));
    ctx->r5 = static_cast<gpr>(static_cast<int32_t>(a1));
    ctx->r6 = 0;
    ctx->r7 = 0;
    MEM_W(0x10, ctx->r29) = 0;
    function(rdram, ctx);
    *ctx = saved;
}

// Makes the scene's characters the ones the point had. The game makes some characters as the scene
// is played (scene 6's three wasps, by an event script) and takes others away, while loading a save
// makes the scene's starting ones; so a character the point had from a placement that has none now
// is made (func_15082A44, as a script does), and one now from a placement the point had none from is
// taken away (func_15060F28), Conker aside. match_records then finds each saved one its slot.
void sync_characters(uint8_t* rdram, recomp_context* ctx, const TableState& actors, std::string* restored) {
    const uint32_t placements = static_cast<uint32_t>(MEM_W(0, kseg0(kPlacements)));
    const uint32_t slots = static_cast<uint32_t>(MEM_W(0, kseg0(kPlacementSlots)));
    const int placement_count = MEM_BU(0, kseg0(kPlacementCount));
    if (!in_rdram(placements) || !in_rdram(slots) || placement_count == 0) {
        return;
    }
    std::vector<int> saved_model(placement_count, -1);  // per placement, the model the point had from it
    for (int i = 0; i < actors.count; i++) {
        const uint8_t* saved = actors.bytes.data() + static_cast<size_t>(i) * kActorSize;
        const int placement = saved[kActorPlacement];
        if ((saved[0] | saved[1] | saved[2] | saved[3]) != 0 && placement < placement_count) {
            saved_model[placement] = saved[actors.spec->model_byte];
        }
    }
    auto call = [&](void (*function)(uint8_t*, recomp_context*), uint32_t a0, uint32_t a1) {
        call_game(rdram, ctx, function, a0, a1);
    };
    std::vector<bool> present(placement_count, false);
    int removed = 0;
    for (int slot = 0; slot < kActorCount; slot++) {
        const uint32_t actor = kActors + static_cast<uint32_t>(slot) * kActorSize;
        const int placement = MEM_BU(kActorPlacement, kseg0(actor));
        if (MEM_W(0, kseg0(actor)) == 0 || placement >= placement_count) {
            continue;
        }
        if (slot != 0 && saved_model[placement] < 0) {
            call(func_15060F28, actor, 0);
            removed++;
        } else {
            present[placement] = true;
        }
    }
    int made = 0;
    for (int placement = 0; placement < placement_count; placement++) {
        const uint32_t record = placements + static_cast<uint32_t>(placement) * kPlacementSize;
        if (saved_model[placement] < 0 || present[placement] || (MEM_BU(placement, kseg0(slots)) & 0x7F) != 0 ||
            MEM_BU(kPlacementModel, kseg0(record)) != saved_model[placement]) {
            continue;
        }
        MEM_B(kPlacementCleared, kseg0(record)) = 0;
        call(func_15082A44, record, static_cast<uint32_t>(placement));
        made++;
    }
    if (made != 0 || removed != 0) {
        *restored += " characters made " + std::to_string(made) + ", taken away " + std::to_string(removed) + ";";
    }
}

// Which record now each saved one goes back over, or -1 (and, for the actors, which slot each saved
// one is in now, for restore_swarms). A scene does not always fill the character slots in the same
// order: loaded again, scene 6's Tediz and the others after them came back shuffled, and put back
// slot by slot five were left out and two Tediz stood twice. So a record that is in use (a nonzero
// first word) goes back over the one with the same model and the same `id_offset` (the low half of
// actor +0x38, 0xD8F000xx, one number per character of the scene), in its own slot if that is the
// one; then any other record over the one in its own slot that holds the same model, as before, or
// that is as empty (a slot freed while playing holds model 0xFF, one never used 0).
std::vector<int> match_records(uint8_t* rdram, const TableSpec& spec, uint32_t records, const TableState& table) {
    const int count = table.count;
    std::vector<int> now_of(count, -1);
    std::vector<bool> taken(count, false);
    auto saved_at = [&](int index) { return table.bytes.data() + static_cast<size_t>(index) * spec.record_size; };
    auto record_at = [&](int index) { return records + static_cast<uint32_t>(index) * spec.record_size; };
    auto in_use = [&](int index) {
        const uint8_t* saved = saved_at(index);
        return (saved[0] | saved[1] | saved[2] | saved[3]) != 0;
    };
    auto fits = [&](int saved_index, int index, bool by_id) {
        const uint8_t* saved = saved_at(saved_index);
        const uint32_t record = record_at(index);
        if (spec.model_byte >= 0 && MEM_BU(spec.model_byte, kseg0(record)) != saved[spec.model_byte]) {
            return false;
        }
        return !by_id || MEM_HU(spec.id_offset, kseg0(record)) ==
                             ((uint32_t{saved[spec.id_offset]} << 8) | saved[spec.id_offset + 1]);
    };
    auto take = [&](int saved_index, int index) {
        now_of[saved_index] = index;
        taken[index] = true;
    };
    if (spec.id_offset >= 0) {
        for (int i = 0; i < count; i++) {
            if (in_use(i) && fits(i, i, true)) {
                take(i, i);
            }
        }
        for (int i = 0; i < count; i++) {
            for (int j = 0; j < count && now_of[i] < 0 && in_use(i); j++) {
                if (!taken[j] && fits(i, j, true)) {
                    take(i, j);
                }
            }
        }
    }
    for (int i = 0; i < count; i++) {
        const bool both_empty = spec.id_offset >= 0 && !in_use(i) && MEM_W(0, kseg0(record_at(i))) == 0;
        if (now_of[i] < 0 && !taken[i] && (fits(i, i, false) || both_empty)) {
            take(i, i);
        }
    }
    return now_of;
}

// Which object now each saved one goes back over, or -1. The scene's object table is not always
// the same length as when it was saved: a pickup taken before the save is made again by a fresh
// load (scene 49 had 19 objects saved and 20 loaded), and put back by index the whole table was
// skipped, its gates with it. So a saved object goes back over the one with its own index, at its
// own place in the table first; one the scene does not have now is left out, and an object the
// save did not have is left as the scene made it.
std::vector<int> match_objects(uint8_t* rdram, const TableSpec& spec, uint32_t records, int count_now,
                               const TableState& table) {
    std::vector<int> now_of(table.count, -1);
    std::vector<bool> taken(count_now, false);
    auto saved_at = [&](int index) { return table.bytes.data() + static_cast<size_t>(index) * spec.record_size; };
    auto fits = [&](int saved_index, int index) {
        const uint8_t* saved = saved_at(saved_index);
        const uint32_t record = records + static_cast<uint32_t>(index) * spec.record_size;
        return MEM_BU(kObjectIndex, kseg0(record)) == saved[kObjectIndex];
    };
    for (int i = 0; i < table.count; i++) {
        if (i < count_now && fits(i, i)) {
            now_of[i] = i;
            taken[i] = true;
        }
    }
    for (int i = 0; i < table.count; i++) {
        for (int j = 0; j < count_now && now_of[i] < 0; j++) {
            if (!taken[j] && fits(i, j)) {
                now_of[i] = j;
                taken[j] = true;
            }
        }
    }
    return now_of;
}

// The link (kActorLink) a restored character gets, from the one saved (`saved`), the one the scene
// gave it now (`now`) and where each saved character went (`now_of`, saved slot -> slot now). The
// slots are shuffled between loads, so a saved link names the slot its character was in then:
// restored as it was, scene 60's bat, saved in slot 5 keeping to Batula's perch in slot 4, came
// back in slot 18 keeping to slot 4, a villager by then, and circled him as a giant bat; and every
// save made after that kept it. So the link the scene set up when it made the character is kept
// (which also mends those saves), and otherwise the saved one follows its character to its slot
// now (a child made during play, say), or is cleared when that character was not restored.
int restored_link(int saved, int now, const std::vector<int>& now_of) {
    if (now != 0 || saved == 0) {
        return now;
    }
    const int slot = saved <= static_cast<int>(now_of.size()) ? now_of[saved - 1] : -1;
    return slot >= 0 ? slot + 1 : 0;
}

// Starts the saved event scripts that are not running. Entering a scene starts only its own list
// (func_1509B4A0); one a trigger, a cutscene or another script started during play (Bats Tower's
// lever, event 0x55) would otherwise be lost, and what it runs never happens.
// Each is started as the script VM's command starts one (func_1509F850: func_1509B5AC(event, 0)),
// which runs its first step now, before the characters are matched to the save, so the ones it
// makes are matched like the rest; its key's flags are put back as they were, and its variables
// further on. Returns how many were started.
int start_scripts(uint8_t* rdram, recomp_context* ctx, const Point& point) {
    int started = 0;
    for (const ScriptVariables& script : point.scripts) {
        const uint16_t event = script.key & kScriptEvent;
        uint32_t found = 0;
        const auto find = [&](uint32_t node) {
            if ((MEM_HU(0, kseg0(node)) & kScriptEvent) == event) {
                found = node;
            }
        };
        for_each_script(rdram, find);
        if (found != 0) {
            continue;
        }
        call_game(rdram, ctx, func_1509B5AC, event, 0);
        for_each_script(rdram, find);
        if (found != 0) {
            MEM_H(0, kseg0(found)) = static_cast<int16_t>(script.key);
            started++;
        }
    }
    return started;
}

// Puts back the scene `point` recorded: each table whose record count is the same as now (an actor
// slot only if it holds the same model), word by word, except the words restorable leaves, and the variables of each event script running again (the same event, the
// same size). The game then carries on from there: objects move to the states they had, a fired
// trigger stays fired. Then the swarms (restore_swarms). The saved scripts not running are started
// first (start_scripts).
void restore_scene(uint8_t* rdram, recomp_context* ctx, const Point& point) {
    std::string restored;
    std::vector<int> actor_slots;  // saved slot -> slot now (match_records), empty if not restored
    const int started = start_scripts(rdram, ctx, point);
    for (const TableState& table : point.tables) {
        if (table.spec->id_offset >= 0) {
            sync_characters(rdram, ctx, table, &restored);
        }
    }
    for (const TableState& table : point.tables) {
        const TableSpec& spec = *table.spec;
        int count;
        const uint32_t records = table_records(rdram, spec, &count);
        if (count != table.count && !spec.by_place) {
            restored += std::string(" ") + spec.name + " (skipped, " + std::to_string(count) + " now, " +
                        std::to_string(table.count) + " saved)";
            continue;
        }
        if (count == 0) {
            restored += std::string(" ") + spec.name + " (skipped, none now)";
            continue;
        }
        const std::vector<int> now_of = spec.by_place ? match_objects(rdram, spec, records, count, table)
                                                      : match_records(rdram, spec, records, table);
        if (spec.id_offset >= 0) {
            actor_slots = now_of;
        }
        int records_restored = 0;
        for (int index = 0; index < table.count; index++) {
            if (now_of[index] < 0) {
                continue;
            }
            const uint32_t record = records + static_cast<uint32_t>(now_of[index]) * spec.record_size;
            const uint8_t* saved = table.bytes.data() + static_cast<size_t>(index) * spec.record_size;
            const int link_now = MEM_BU(kActorLink, kseg0(record));
            for (uint32_t offset = 0; offset < spec.record_size; offset += 4) {
                const uint32_t then = (uint32_t{saved[offset]} << 24) | (uint32_t{saved[offset + 1]} << 16) |
                                      (uint32_t{saved[offset + 2]} << 8) | saved[offset + 3];
                const uint32_t now = static_cast<uint32_t>(MEM_W(0, kseg0(record + offset)));
                if (restorable(then, now)) {
                    MEM_W(0, kseg0(record + offset)) = static_cast<int32_t>(then);
                }
            }
            if (spec.id_offset >= 0) {
                MEM_B(kActorLink, kseg0(record)) = static_cast<int8_t>(restored_link(saved[kActorLink], link_now, now_of));
            }
            records_restored++;
        }
        restored += std::string(" ") + spec.name + " " + std::to_string(records_restored) + "/" + std::to_string(table.count);
        if (count != table.count) {
            restored += " (" + std::to_string(count) + " now)";
        }
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
    if (point.has_swarms) {
        restore_swarms(rdram, ctx, point, actor_slots, &restored);
    }
    if (const uint32_t player = player_block(rdram); player != 0) {
        char kept[64];
        if (point.actions[0] != 0 || point.actions[1] != 0) {
            for (size_t i = 0; i < std::size(kPlayerActions); i++) {
                if (point.actions[i] != 0) {
                    MEM_B(kPlayerActions[i], kseg0(player)) = static_cast<int8_t>(point.actions[i]);
                }
            }
            std::snprintf(kept, sizeof(kept), " Conker's actions 0x%02X 0x%02X", point.actions[0], point.actions[1]);
            restored += kept;
        }
        if (point.drunk > 0) {
            MEM_H(kPlayerDrunk, kseg0(player)) = static_cast<int16_t>(point.drunk);
            std::snprintf(kept, sizeof(kept), " drunk %d frames", point.drunk);
            restored += kept;
        }
    }
    char tail[64];
    std::snprintf(tail, sizeof(tail), ", event scripts %d/%zu (%d started)", scripts, point.scripts.size(), started);
    restored += tail;
    std::printf("[conker] resumed scene state:%s\n", restored.c_str());
    check::restored(rdram, restored);
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

float camera_distance(const Camera& camera) {
    const float d[3] = {camera.at[0] - camera.eye[0], camera.at[1] - camera.eye[1], camera.at[2] - camera.eye[2]};
    return std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
}

// Measures how far the camera is from the point it looks at, and its pitch, for place_view.
void measure_camera(uint8_t* rdram) {
    Camera camera;
    if (current_camera(rdram, &camera)) {
        g_camera_distance = camera_distance(camera);
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

// Makes the view's distance the one the game's camera keeps (see kTargetDistance).
void keep_distance(uint8_t* rdram, uint32_t object) {
    const uint32_t control = static_cast<uint32_t>(MEM_W(0, kseg0(object + kObjectCameraControl)));
    if ((control & 0xFF800000) != 0x80000000) {
        return;
    }
    const float ground = g_camera_distance * std::cos(g_camera_pitch * (3.14159265f / 180.0f));
    write_float(rdram, control + kTargetDistance, ground);
    if (read_float(rdram, control + kZoomKeptDistance) != -1.0f) {
        write_float(rdram, control + kZoomKeptDistance, ground);
    }
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
    // At 60 fps only every second frame counts, so the waits below last as long as at 30 and the
    // camera's movement is measured over the same time; a held view is still held on every frame.
    const bool step = conker::frame_rate::thirty_hz_step(rdram);
    if (!step) {
        if (g_camera_hold > 0 && object != 0) {
            const float hx = read_float(rdram, object + kObjectX) - g_view.x;
            const float hz = read_float(rdram, object + kObjectZ) - g_view.z;
            place_view(rdram, object, g_view, hx * hx + hz * hz < 1.0f, true);
        }
        return;
    }
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
        if (g_camera_saved) {
            keep_distance(rdram, object);
        }
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
        if (!g_camera_saved) {
            measure_camera(rdram);
        }
        place_view(rdram, object, g_view, where_saved, true);
        if (g_camera_saved) {
            keep_distance(rdram, object);
        }
        g_camera_hold = kPlaceFrames;
        std::printf("[conker] resumed camera: yaw %.2f, distance %.2f, pitch %.2f%s\n", g_view.camera_yaw,
                    g_camera_distance, g_camera_pitch, g_camera_saved ? "" : " (as the game placed it)");
    }
}

// Once a frame while the cover is black: takes it away once the started save's scene is running
// with Conker placed. The circle wipe that opens a scene holds a small circle around him for about
// a second, which the cover keeps hidden; it goes as the wipe opens, so the wipe shows him. Where
// there is no wipe, it fades in.
void update_cover(uint8_t* rdram) {
    if (g_cover != Cover::Black) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    const bool changing = MEM_BU(0, kseg0(kTransition)) != 0;
    g_cover_changed = g_cover_changed || (g_cover_started && changing);
    const bool in_place = g_cover_changed && !changing && !g_load_requested && !g_resuming.valid &&
                          player_object(rdram) != 0;
    g_cover_frames = in_place ? g_cover_frames + 1 : 0;
    const bool wipe_opening = read_float(rdram, kWipeSpeed) < kWipeOpening;
    if (in_place && wipe_opening) {
        g_cover = Cover::None;
    } else if ((g_cover_frames >= kCoverFrames && read_float(rdram, kWipe) == 0.0f) || now - g_cover_since > kCoverLimit) {
        g_cover = Cover::FadingIn;
        g_cover_since = now;
    }
}

}  // namespace

int32_t load_cover() {
    if (g_cover == Cover::Black) {
        g_cover_drawn = true;
        return 0xFF;
    }
    if (g_cover != Cover::FadingIn) {
        return 0;
    }
    const auto elapsed = std::chrono::steady_clock::now() - g_cover_since;
    if (elapsed >= kCoverFade) {
        g_cover = Cover::None;
        return 0;
    }
    return static_cast<int32_t>(0xFF - 0xFF * elapsed / kCoverFade);
}

// Records where Conker is and the scene as it is now in `point`; false when there is no Conker.
bool capture_point(uint8_t* rdram, Point* point) {
    const uint32_t object = static_cast<uint32_t>(MEM_W(0, kseg0(kPlayerObject)));
    if ((object & 0xFF000000) != 0x80000000) {
        return false;
    }
    *point = Point{};
    point->valid = true;
    point->scene = MEM_W(0, kseg0(kScene));
    point->entrance = MEM_BU(0, kseg0(kEntrance));
    point->x = read_float(rdram, object + kObjectX);
    point->y = read_float(rdram, object + kObjectY);
    point->z = read_float(rdram, object + kObjectZ);
    point->yaw = read_float(rdram, object + kObjectYaw) * (180.0f / 3.14159265f);
    Position here;
    point->has_view = current_position(rdram, &here) && here.has_view;
    point->facing = here.facing;
    point->camera_yaw = here.camera_yaw;
    Camera camera;
    if (point->has_view && current_camera(rdram, &camera)) {
        point->has_camera = true;
        point->camera_distance = camera_distance(camera);
        point->camera_pitch = camera.pitch();
    }
    capture_scene(rdram, point);
    return true;
}

// A save made during a cutscene is the scene as the cutscene found it, which plays again from its
// start once the save is resumed: the cutscene itself cannot be taken up partway. So the scene is
// recorded on the first frame of every cutscene (from conker_resume_tick).
void note_cutscene(uint8_t* rdram) {
    const bool playing = MEM_BU(0, kseg0(kCutscenePlaying)) == 1;
    if (playing && !g_cutscene_was_playing && !g_resuming.valid && capture_point(rdram, &g_cutscene_start)) {
        g_cutscene_start.cutscene = MEM_BU(0, kseg0(kCutsceneScript));
    } else if (!playing) {
        g_cutscene_start.valid = false;
    }
    g_cutscene_was_playing = playing;
}

void remember_position(uint8_t* rdram, int slot) {
    if (slot < 0 || slot >= kSlots) {
        return;
    }
    if (MEM_BU(0, kseg0(kCutscenePlaying)) == 1 && g_cutscene_start.valid &&
        g_cutscene_start.scene == MEM_W(0, kseg0(kScene))) {
        g_pending = g_cutscene_start;
    } else if (!capture_point(rdram, &g_pending)) {
        return;
    }
    g_pending_slot = slot;
}

// A load reads a record, not a slot (func_15006590(D_800BE3EC) reads blocks 4 + 16 * record): the
// one the slot is noted in, as the title menu asks for it (func_150076BC(D_800BE3F8[slot] +0xE)).
// Asking for the slot's own number read whatever record had that number, often an older save of
// the slot (see note_records).
void request_load(uint8_t* rdram, int slot) {
    if (slot < 0 || slot >= kSlots) {
        return;
    }
    const int record = MEM_B(kSlotRecord, kseg0(kSaveCounters + static_cast<uint32_t>(slot) * 16));
    if (record < 0 || record >= kRecords) {
        std::printf("[conker] slot %d has no save to load\n", slot);
        return;
    }
    MEM_B(0, kseg0(kEepromAction)) = kEepromLoad;
    MEM_B(0, kseg0(kEepromSlot)) = static_cast<int8_t>(record);
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
        g_camera_saved = false;
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

// The event flags `slot` holds in the save `bin`, or none when it is not a whole save or the slot's
// bytes are not that slot's.
std::vector<uint8_t> saved_events(const std::filesystem::path& bin, int slot) {
    std::array<char, kEepromSize> image{};
    std::ifstream file(bin, std::ios::binary);
    if (slot < 0 || slot >= kSlots || !file.read(image.data(), image.size())) {
        return {};
    }
    const char* save = image.data() + kSlotSave + slot * kSlotSaveSize;
    if (static_cast<uint8_t>(save[kSavedSlot]) != slot) {
        return {};
    }
    return std::vector<uint8_t>(save + kSavedEvents, save + kSavedEvents + kEventBytes);
}

// The saved slot: a snapshot's .resume has its point first, a report's report.json names it.
int snapshot_slot(const std::filesystem::path& resume) {
    std::ifstream file(resume);
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::array<Point, kSlots> points;
    int first;
    std::string problem;
    return parse_points(text, &points, &first, &problem) ? first : -1;
}

int report_slot(const std::filesystem::path& folder) {
    std::ifstream file(folder / "report.json");
    const nlohmann::json doc = nlohmann::json::parse(file, nullptr, false);
    if (!doc.is_object() || !doc.contains("save") || !doc["save"].is_object() || !doc["save"].contains("slot") ||
        !doc["save"]["slot"].is_number_integer()) {
        return -1;
    }
    return doc["save"]["slot"].get<int>();
}

}  // namespace

std::filesystem::path data_folder() {
    // The save file's folder's folder, taken before a report's save can move the save file
    // (conker_resume_tick asks once a frame from the start).
    static const std::filesystem::path folder = ultramodern::get_save_file_path().parent_path().parent_path();
    return folder;
}

std::filesystem::path player_save_path() {
    // As data_folder: the player's own save file, before a report's save moves the game off it.
    static const std::filesystem::path path = ultramodern::get_save_file_path();
    return path;
}

std::vector<Snapshot> list_snapshots() {
    std::vector<Snapshot> snapshots;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(snapshots_folder(), error)) {
        Snapshot snapshot;
        snapshot.path = entry.path();
        if (snapshot.path.extension() == ".bin" && parse_name(snapshot.path.stem().string(), &snapshot)) {
            snapshot.events = saved_events(snapshot.path,
                                           snapshot_slot(std::filesystem::path(snapshot.path).replace_extension(".resume")));
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
            report.events = saved_events(report.path, report_slot(entry.path()));
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
    // A report made during a cutscene has the scene as the cutscene found it (note_cutscene), so its
    // point is where Conker was then, not where the report found him.
    const Point& point = points[first_slot];
    const bool where = std::fabs(point.x - x) < 0.01 && std::fabs(point.y - y) < 0.01 && std::fabs(point.z - z) < 0.01;
    const bool found = point.counter == static_cast<uint32_t>(counter) && point.scene == static_cast<int>(scene) &&
                       (where || point.cutscene >= 0);
    if (!found) {
        *problem = "save.resume does not match report.json";
        return false;
    }
    return true;
}

// A point saved before points recorded the camera's distance and pitch: they are taken from the
// report's camera (report.json "camera": "eye" and "look_at", recorded as the save was made), so the
// view is the one reported, not wherever the scene's entrance camera comes to rest. False when the
// point has them already or the report has no usable camera.
bool report_camera(const nlohmann::json& report, Point* point) {
    if (!point->valid || !point->has_view || point->has_camera) {
        return false;
    }
    const nlohmann::json camera = report.value("camera", nlohmann::json());
    if (!camera.is_object()) {
        return false;
    }
    const nlohmann::json eye = camera.value("eye", nlohmann::json());
    const nlohmann::json at = camera.value("look_at", nlohmann::json());
    if (!eye.is_array() || !at.is_array() || eye.size() != 3 || at.size() != 3) {
        return false;
    }
    float d[3];
    for (int i = 0; i < 3; i++) {
        if (!eye[i].is_number() || !at[i].is_number() || std::fabs(eye[i].get<double>()) > kMaxCoordinate ||
            std::fabs(at[i].get<double>()) > kMaxCoordinate) {
            return false;
        }
        d[i] = static_cast<float>(at[i].get<double>() - eye[i].get<double>());
    }
    const float distance = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(distance >= kMinCameraDistance)) {
        return false;
    }
    point->has_camera = true;
    point->camera_distance = distance;
    point->camera_pitch = std::asin(std::clamp(d[1] / distance, -1.0f, 1.0f)) * (180.0f / 3.14159265f);
    return true;
}

}  // namespace

bool load_report(uint8_t* rdram, const std::filesystem::path& path, std::string* problem, bool at_checkpoint) {
    if (MEM_B(0, kseg0(kEepromAction)) != 0 || MEM_W(0, kseg0(kEepromWriting)) != 0 || g_snapshot_pending ||
        g_keep_pending || g_report.active) {
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
    player_save_path();
    ultramodern::change_save_file(u8"debug", std::u8string(name.begin(), name.end()));
    g_on_report_copy = true;
    int slot;
    std::array<Point, kSlots> report_points = read_points(points_path(), &slot);
    if (slot >= 0 && report_camera(files.report, &report_points[slot])) {
        write_points(report_points, points_path(), slot);
    }
    start_save(rdram, report_points, slot);
    g_at_checkpoint = at_checkpoint;
    std::printf("[conker] loading debug report %s%s, slot %d, playing on %s until the game is closed\n",
                folder.string().c_str(), at_checkpoint ? " at its checkpoint" : "", MEM_B(0, kseg0(kSaveSlot)),
                copy.string().c_str());
    return true;
}

namespace {

// Starts `slot` of the save just put in place, as LOAD does (the first point's slot if there is one).
// The save keeps each slot in one of four 0x80-byte records (EEPROM blocks 4 + 16 * record), written
// in turn, the newest a slot's: {u16 checksum, slot, u32 sequence number, event flags, ...}. Reading
// the EEPROM at boot, the game notes which record holds each slot and its sequence number
// (D_800BE3F8 + 16 * slot: +8 and +0xE), and which slot each record holds (D_800BE3D8[record]); LOAD
// reads the slot from the record noted (func_15006590), and SAVE writes to one no slot holds. A save
// put in place afterwards (a snapshot, a report's copy) has records of its own, so they are noted
// again from it, as the game notes them: each slot's newest record whose checksum holds. Without
// this the game read the slot from the record the booted save had it in, an older save of the same
// slot in the copy, whose event flags lagged behind its point's scene (in Bats Tower the cog girls
// and later the dogfish never came out). Returns the slot's sequence number, or -1 when it has none.
int64_t note_records(uint8_t* rdram, int slot) {
    save_read(rdram, static_cast<int32_t>(kSnapshotScratch), 0, kEepromSize);
    const auto byte = [&](uint32_t offset) { return MEM_BU(static_cast<int>(offset), kseg0(kSnapshotScratch)); };
    int newest[kSlots] = {-1, -1, -1};
    uint32_t counters[kSlots] = {};
    for (int record = 0; record < kRecords; record++) {
        const uint32_t base = (kFirstRecordBlock + static_cast<uint32_t>(record) * kRecordBlocks) * 8;
        uint32_t sum = kRecordChecksumStart;
        for (uint32_t i = 2; i < kRecordSize; i++) {
            sum = (sum + (static_cast<uint32_t>(byte(base + i)) << (i & 3))) & 0xFFFF;
        }
        const int owner = byte(base + 2);
        const uint32_t counter = (static_cast<uint32_t>(byte(base + 3)) << 24) | (byte(base + 4) << 16) |
                                 (byte(base + 5) << 8) | byte(base + 6);
        if (sum != ((static_cast<uint32_t>(byte(base)) << 8) | byte(base + 1)) || owner >= kSlots) {
            continue;
        }
        if (newest[owner] < 0 || counter > counters[owner]) {
            newest[owner] = record;
            counters[owner] = counter;
        }
    }
    for (int record = 0; record < kRecords; record++) {
        MEM_B(record, kseg0(kRecordSlots)) = -1;
    }
    for (int s = 0; s < kSlots; s++) {
        const uint32_t entry = kSaveCounters + static_cast<uint32_t>(s) * 16;
        MEM_W(0, kseg0(entry)) = newest[s] < 0 ? -1 : static_cast<int32_t>(counters[s]);
        MEM_B(kSlotRecord, kseg0(entry)) = static_cast<int8_t>(newest[s]);
        if (newest[s] >= 0) {
            MEM_B(newest[s], kseg0(kRecordSlots)) = static_cast<int8_t>(s);
        }
    }
    return newest[slot] < 0 ? -1 : counters[slot];
}

void start_save(uint8_t* rdram, const std::array<Point, kSlots>& points, int slot) {
    if (slot < 0) {
        slot = MEM_B(0, kseg0(kSaveSlot)) >= 0 ? MEM_B(0, kseg0(kSaveSlot)) : 0;
    }
    const int64_t counter = note_records(rdram, slot);
    if (counter != points[slot].counter && points[slot].valid) {
        std::printf("[conker] slot %d's newest save is %lld, its point is for %u: starting at its checkpoint\n", slot,
                    static_cast<long long>(counter), points[slot].counter);
    }
    MEM_B(0, kseg0(kSaveSlot)) = static_cast<int8_t>(slot);
    request_load(rdram, slot);
    g_cover = Cover::Black;
    g_cover_drawn = false;
    g_cover_black = 0;
    g_cover_started = false;
    g_cover_changed = false;
    g_cover_frames = 0;
    g_cover_since = std::chrono::steady_clock::now();
}

// Save records in a whole EEPROM image (see note_records).
uint8_t* record_at(std::array<uint8_t, kEepromSize>& image, int record) {
    return image.data() + (kFirstRecordBlock + static_cast<uint32_t>(record) * kRecordBlocks) * 8;
}

uint32_t record_sum(const uint8_t* record) {
    uint32_t sum = kRecordChecksumStart;
    for (uint32_t i = 2; i < kRecordSize; i++) {
        sum = (sum + (static_cast<uint32_t>(record[i]) << (i & 3))) & 0xFFFF;
    }
    return sum;
}

// The slot `record` holds, or -1 when its checksum does not hold.
int record_slot(const uint8_t* record) {
    if (record_sum(record) != ((static_cast<uint32_t>(record[0]) << 8) | record[1]) || record[kSavedSlot] >= kSlots) {
        return -1;
    }
    return record[kSavedSlot];
}

uint32_t record_counter(const uint8_t* record) {
    return (static_cast<uint32_t>(record[3]) << 24) | (record[4] << 16) | (record[5] << 8) | record[6];
}

// Each slot's newest record in `image`, -1 for none.
std::array<int, kSlots> newest_records(std::array<uint8_t, kEepromSize>& image) {
    std::array<int, kSlots> newest = {-1, -1, -1};
    for (int record = 0; record < kRecords; record++) {
        const int slot = record_slot(record_at(image, record));
        if (slot >= 0 && (newest[slot] < 0 || record_counter(record_at(image, record)) >
                                                  record_counter(record_at(image, newest[slot])))) {
            newest[slot] = record;
        }
    }
    return newest;
}

bool read_image(const std::filesystem::path& path, std::array<uint8_t, kEepromSize>* image) {
    std::ifstream file(path, std::ios::binary);
    return file.read(reinterpret_cast<char*>(image->data()), kEepromSize) && file.gcount() == kEepromSize;
}

// Playing on a debug report's copy of the save, a game saved from the pause menu goes into the
// player's own save too, or it would be gone once the game is closed (the copy is only played
// until then). The slot's newest record is written over a record of the player's save that is no
// slot's newest, numbered after every save of the slot there, and its point goes into the player's
// .resume with that number; the player's other games, and the rest of the EEPROM, are kept. With no
// save of the player's yet, the copy is taken without the report's other games.
void keep_in_player_save(uint8_t* rdram) {
    save_read(rdram, static_cast<int32_t>(kSnapshotScratch), 0, kEepromSize);
    std::array<uint8_t, kEepromSize> copy;
    for (uint32_t i = 0; i < kEepromSize; i++) {
        copy[i] = MEM_BU(static_cast<int>(i), kseg0(kSnapshotScratch));
    }
    const int slot = g_keep_slot;
    const int saved = newest_records(copy)[slot];
    if (saved < 0) {
        std::printf("[conker] slot %d has no save in the report's copy; the player's save is left as it was\n", slot);
        return;
    }
    const std::filesystem::path path = player_save_path();
    std::array<uint8_t, kEepromSize> image;
    if (!read_image(path, &image)) {
        image = copy;
        for (int record = 0; record < kRecords; record++) {
            if (record_slot(record_at(image, record)) != slot) {
                std::memset(record_at(image, record), 0, kRecordSize);
            }
        }
    }
    const std::array<int, kSlots> newest = newest_records(image);
    int target = -1;
    uint32_t counter = record_counter(record_at(copy, saved));
    bool numbered = false;
    for (int record = 0; record < kRecords; record++) {
        const uint8_t* bytes = record_at(image, record);
        if (std::find(newest.begin(), newest.end(), record) == newest.end() && target < 0) {
            target = record;
        }
        if (record_slot(bytes) == slot && (!numbered || record_counter(bytes) >= counter)) {
            counter = (record_counter(bytes) + 1) & 0x7FFFFFFF;
            numbered = true;
        }
    }
    uint8_t* record = record_at(image, target);  // three slots, four records: one is always free
    std::memcpy(record, record_at(copy, saved), kRecordSize);
    record[3] = static_cast<uint8_t>(counter >> 24);
    record[4] = static_cast<uint8_t>(counter >> 16);
    record[5] = static_cast<uint8_t>(counter >> 8);
    record[6] = static_cast<uint8_t>(counter);
    const uint32_t sum = record_sum(record);
    record[0] = static_cast<uint8_t>(sum >> 8);
    record[1] = static_cast<uint8_t>(sum);

    std::filesystem::path temp = path;
    temp += ".keep";
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        if (!file.write(reinterpret_cast<const char*>(image.data()), image.size())) {
            std::printf("[conker] could not write %s\n", temp.string().c_str());
            return;
        }
    }
    std::filesystem::rename(temp, path, error);
    if (error) {
        std::printf("[conker] could not replace %s: %s\n", path.string().c_str(), error.message().c_str());
        return;
    }
    std::filesystem::path resume = path;
    resume.replace_extension(".resume");
    std::array<Point, kSlots> points = read_points(resume);
    points[slot] = g_keep_point;
    points[slot].counter = counter;
    write_points(points, resume);
    std::printf("[conker] slot %d's save kept in %s too (record %d, save %u)\n", slot, path.string().c_str(), target,
                counter);
}

}  // namespace

namespace check {
void restoring_hook(uint8_t* rdram);  // save_check.cpp
void restored_hook(uint8_t* rdram);
void count_frame();
namespace {
int g_frames_since_restore = -1;
std::string g_restore_summary;
}  // namespace

bool write_capture(uint8_t* rdram, const std::filesystem::path& path) {
    Point point;
    if (!capture_point(rdram, &point)) {
        return false;
    }
    const int slot = std::clamp<int>(MEM_B(0, kseg0(kSaveSlot)), 0, kSlots - 1);
    point.counter = save_counter(rdram, slot);
    std::array<Point, kSlots> points{};
    points[slot] = std::move(point);
    write_points(points, path, slot);
    // Added to the file: the running scripts, whatever their variables, and where this is in time.
    std::ifstream in(path);
    nlohmann::json doc = nlohmann::json::parse(in, nullptr, false);
    in.close();
    if (!doc.is_object()) {
        return false;
    }
    nlohmann::json keys = nlohmann::json::array();
    for_each_script(rdram, [&](uint32_t node) { keys.push_back(MEM_HU(0, kseg0(node))); });
    doc["script_keys"] = std::move(keys);
    doc["frame"] = g_frames_since_restore;
    doc["restore"] = g_restore_summary;
    doc["cutscene_playing"] = MEM_BU(0, kseg0(kCutscenePlaying)) == 1;
    doc["transition"] = MEM_BU(0, kseg0(kTransition));
    std::ofstream(path, std::ios::trunc) << doc.dump(1) << '\n';
    return true;
}

void restoring(uint8_t* rdram) { restoring_hook(rdram); }

void restored(uint8_t* rdram, const std::string& summary) {
    g_frames_since_restore = 0;
    g_restore_summary = summary;
    restored_hook(rdram);
}

void count_frame() {
    if (g_frames_since_restore >= 0) {
        g_frames_since_restore++;
    }
}

}  // namespace check

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
    if (g_on_report_copy) {
        g_keep_pending = true;
        g_keep_slot = slot;
        g_keep_point = points[slot];
    }
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
    if (g_at_checkpoint) {
        g_at_checkpoint = false;
        std::printf("[conker] starting slot %d at its checkpoint\n", slot);
        return;
    }
    if (slot < 0 || slot >= kSlots) {
        return;
    }
    const Point point = read_points()[slot];
    if (!point.valid || point.counter != save_counter(rdram, slot)) {
        return;
    }
    g_checkpoint[0] = MEM_B(0, kseg0(kCheckpointScene));
    g_checkpoint[1] = MEM_BU(0, kseg0(kCheckpointEntrance));
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
        g_camera_saved = point.has_camera && point.camera_distance >= kMinCameraDistance;
        if (g_camera_saved) {
            g_camera_distance = point.camera_distance;
            g_camera_pitch = point.camera_pitch;
        }
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
    player_save_path();
    check::tick(rdram, ctx);
    if (g_snapshot_pending && MEM_B(0, kseg0(kEepromAction)) == 0 && MEM_W(0, kseg0(kEepromWriting)) == 0) {
        g_snapshot_pending = false;
        write_snapshot(rdram);
    }
    if (g_keep_pending && MEM_B(0, kseg0(kEepromAction)) == 0 && MEM_W(0, kseg0(kEepromWriting)) == 0) {
        g_keep_pending = false;
        keep_in_player_save(rdram);
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
    if (g_cover_drawn) {
        g_cover_drawn = false;
        g_cover_black++;
    }
    const bool cover_ready = g_cover != Cover::Black || g_cover_black >= kCoverBlackFrames ||
                             std::chrono::steady_clock::now() - g_cover_since > kCoverLimit;
    if (g_load_requested && MEM_B(0, kseg0(kEepromAction)) == 0 && cover_ready) {
        g_load_requested = false;
        g_cover_started = true;
        recomp_context saved = *ctx;
        func_151E327C(rdram, ctx);
        *ctx = saved;
        return;
    }
    note_cutscene(rdram);
    settle_camera(rdram);
    update_cover(rdram);
    if (!g_resuming.valid) {
        return;
    }
    const uint32_t object = static_cast<uint32_t>(MEM_W(0, kseg0(kPlayerObject)));
    if (MEM_W(0, kseg0(kScene)) != g_resuming.scene || MEM_BU(0, kseg0(kTransition)) != 0 ||
        (object & 0xFF000000) != 0x80000000) {
        return;
    }
    // At 60 fps only every second frame counts (see settle_camera); Conker is still held on the others.
    if (!conker::frame_rate::thirty_hz_step(rdram)) {
        if (g_resume_frames > kSettleFrames) {
            place(rdram, object, g_resuming);
            if (g_view.has_view) {
                place_view(rdram, object, g_view, true, !g_camera_waits);
            }
        }
        return;
    }
    if (++g_resume_frames <= kSettleFrames) {
        return;
    }
    if (g_resume_frames == kSettleFrames + 1) {
        if (g_checkpoint[0] >= 0) {
            MEM_B(0, kseg0(kCheckpointScene)) = static_cast<int8_t>(g_checkpoint[0]);
            MEM_B(0, kseg0(kCheckpointEntrance)) = static_cast<int8_t>(g_checkpoint[1]);
            g_checkpoint[0] = g_checkpoint[1] = -1;
        }
        if (!g_resuming.tables.empty() || !g_resuming.scripts.empty() || g_resuming.has_swarms) {
            check::restoring(rdram);
            restore_scene(rdram, ctx, g_resuming);
        }
    }
    place(rdram, object, g_resuming);
    if (g_view.has_view) {
        place_view(rdram, object, g_view, true, !g_camera_waits);
    }
    if (g_resume_frames >= kSettleFrames + g_place_frames) {
        g_resuming.valid = false;
        if (g_resuming.cutscene >= 0) {
            g_camera_waits = false;  // the cutscene has the camera
            std::printf("[conker] playing cutscene %d of scene %d again\n", g_resuming.cutscene, g_resuming.scene);
            call_game(rdram, ctx, func_1501D348, static_cast<uint32_t>(g_resuming.scene),
                      static_cast<uint32_t>(g_resuming.cutscene));
        }
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
