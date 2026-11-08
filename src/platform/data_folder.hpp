// The game's data folder, conker-recomp-data: the stored ROM, saves, settings, online keys.
#pragma once

#include <filesystem>

namespace conker {

// In the working folder (a desktop launch enters its data location first, src/platform/launch.hpp). A PS5
// title has no working folder, and std::filesystem::current_path() ends it: there the title folder.
inline std::filesystem::path data_folder() {
#if CONKER_PS5
    return std::filesystem::path("/app0") / "conker-recomp-data";
#else
    return std::filesystem::current_path() / "conker-recomp-data";
#endif
}

}  // namespace conker
