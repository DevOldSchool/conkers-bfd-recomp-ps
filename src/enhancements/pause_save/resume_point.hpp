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

// Where Conker is: the scene and his position. `yaw` is the angle a resumed save restores (in
// degrees; object +0xB8), `facing` the way he faces (object +0x40), in degrees from +Z towards +X,
// as `camera_yaw`, the way the camera looks, is. A teleport restores the facing and the camera's
// turn when `has_view` is set.
struct Position {
    int scene = 0;
    float x = 0, y = 0, z = 0, yaw = 0;
    bool has_view = false;
    float facing = 0;
    float camera_yaw = 0;
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
};

// The snapshots, newest first.
std::vector<Snapshot> list_snapshots();

// Makes `path` the save (conker_us.bin and conker_us.resume) and starts its slot where it was
// saved, as LOAD does. False while a save is being written, or when the file cannot be read.
bool load_snapshot(uint8_t* rdram, const std::filesystem::path& path);

}  // namespace conker::resume
