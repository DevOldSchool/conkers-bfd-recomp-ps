#pragma once
// Exact-position saves: see src/enhancements/pause_save/resume_point.cpp.
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "recomp.h"

namespace conker::resume {

// conker-recomp-data: where the reports, spots and display settings are, wherever the save file is
// (a loaded report's save is in saves/debug/).
std::filesystem::path data_folder();
std::filesystem::path player_save_path();

// Remember where Conker is now; it is stored with the next save the game writes for `slot`.
void remember_position(uint8_t* rdram, int slot);

// Reload `slot` from the save and start it where it was saved, as PLAY on the title menu does.
void request_load(uint8_t* rdram, int slot);

// Where Conker is: the scene and his position. `yaw` is the angle a resumed save restores (in
// degrees; object +0xB8), `facing` the way he faces (object +0x40), in degrees from +Z towards +X,
// as `camera_yaw`, the way the camera looks, is. A teleport restores the facing and the camera's
// yaw when `has_view` is set. `camera_pitch` (degrees, up positive) is only recorded: the game keeps
// the camera at the height it wants, so the pitch follows from where Conker is.
struct Position {
    int scene = 0;
    float x = 0, y = 0, z = 0, yaw = 0;
    bool has_view = false;
    float facing = 0;
    float camera_yaw = 0;
    float camera_pitch = 0;
};

// The camera Conker is seen through: where it is, the point it looks at (above Conker), and the
// unit vector from one to the other.
struct Camera {
    float eye[3] = {};
    float at[3] = {};
    float forward[3] = {};
    float yaw() const;    // degrees from +Z towards +X, 0 to 360
    float pitch() const;  // degrees, up positive
};

// False while there is no camera (no Conker).
bool current_camera(uint8_t* rdram, Camera* out);

// False while there is no Conker (a menu, a scene change).
bool current_position(uint8_t* rdram, Position* out);

// Moves Conker to `to` in the scene being played, and holds him there for a few frames. False
// when there is no Conker or `to` is in another scene.
bool teleport(uint8_t* rdram, const Position& to);

// A snapshot of a save in saves/snapshots/ (see resume_point.cpp).
struct Snapshot {
    std::filesystem::path path;  // the .bin
    int scene = 0;
    std::string when;            // "YYYY-MM-DD HH:MM:SS"
    std::string order;           // sorts by when it was made
    std::vector<uint8_t> events; // the saved slot's event flags (D_800D2E4C, a bit per event), or
                                 // none when they could not be read
};

// The snapshots, newest first.
std::vector<Snapshot> list_snapshots();

// The debug reports in conker-recomp-data/debug_reports/, newest first: `path` is the report's
// save.bin (missing when the report has none).
std::vector<Snapshot> list_reports();

// Starts a debug report's save (`path`, its save.bin) where and as it was made, without touching
// the player's save: the game plays on a copy, saves/debug/<report>.bin, until it is closed. A report
// may come from anyone, so it is checked first (files, sizes, report.json's fields and ranges, and
// save.resume against them); when it is refused, or a save is still being written, false with the
// reason in `problem`. `at_checkpoint` starts the save at its checkpoint instead, as the game itself
// does, without the report's position and scene: to tell what the restore does from what the game does.
bool load_report(uint8_t* rdram, const std::filesystem::path& path, std::string* problem, bool at_checkpoint = false);

// Makes a debug report: a folder in debug_reports/ with report.json (`report`, a JSON object, with
// "save" added) and a save of the game as it is now (save.bin, save.resume), made by the game's own
// save routine, as SAVE in the pause menu makes it, into a copy: the player's save is not touched.
// Where the game does not save, report.json only. Returns the folder's name, or "" while a save is
// still being written.
std::string request_report(uint8_t* rdram, const std::string& report);

// Makes `path` the save (conker_us.bin and conker_us.resume) and starts its slot where it was
// saved, as LOAD does. False while a save is being written, or when the file cannot be read.
bool load_snapshot(uint8_t* rdram, const std::filesystem::path& path);

// How black the screen is kept while a save load_snapshot or load_report started is loading, 0
// (clear) to 0xFF; it fades in once Conker is placed. For the game's viewport fade (intro_skip.cpp).
int32_t load_cover();

// For the save/load checks (save_check.cpp, tools/save_check.py).
namespace check {

// Writes the scene as it is now to `path` as a one-slot points file (the running slot's point, as a
// save made now would record it) with "script_keys", every running event script's key, "frame",
// the count of frames since the last restore (-1 before one), and "restore", the summary the last
// restore printed. False when there is no Conker.
bool write_capture(uint8_t* rdram, const std::filesystem::path& path);

// Once a frame from conker_resume_tick, and once a restore has put the scene back (restored).
void tick(uint8_t* rdram, recomp_context* ctx);
void restoring(uint8_t* rdram);  // just before, with the scene as the game loaded it
void restored(uint8_t* rdram, const std::string& summary);

}  // namespace check

}  // namespace conker::resume
