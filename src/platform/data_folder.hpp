// The game's data folder, conker-recomp-data: the stored ROM, saves, settings, online keys.
#pragma once

#include <filesystem>

namespace conker {

// In the working folder (a desktop launch enters its data location first, src/platform/launch.hpp).
// Every part of the game finds it here, so a platform with no working folder changes only this.
inline std::filesystem::path data_folder() {
    return std::filesystem::current_path() / "conker-recomp-data";
}

}  // namespace conker
