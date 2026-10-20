#pragma once
// Exact-position saves: see src/enhancements/pause_save/resume_point.cpp.
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace conker::resume {

// Remember where Conker is now; it is stored with the next save the game writes for `slot`.
void remember_position(uint8_t* rdram, int slot);

// Reload `slot` from the save and start it where it was saved, as PLAY on the title menu does.
void request_load(uint8_t* rdram, int slot);

// Where Conker is: the scene, his position and his facing in degrees.
struct Position {
    int scene = 0;
    float x = 0, y = 0, z = 0, yaw = 0;
};

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
};

// The snapshots, newest first.
std::vector<Snapshot> list_snapshots();

// Makes `path` the save (conker_us.bin and conker_us.resume) and starts its slot where it was
// saved, as LOAD does. False while a save is being written, or when the file cannot be read.
bool load_snapshot(uint8_t* rdram, const std::filesystem::path& path);

}  // namespace conker::resume
