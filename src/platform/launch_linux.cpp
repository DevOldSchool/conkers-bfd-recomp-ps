// The Linux build started from the desktop: data folder and log (see launch.hpp). The ROM dialog is
// in launch_nfd.cpp.
#include "platform/launch.hpp"

#include <cstdio>
#include <cstdlib>
#include <system_error>

#include <unistd.h>

namespace conker::launch {

bool from_desktop() {
    return !isatty(STDERR_FILENO);
}

void enter_data_folder() {
    std::error_code error;
    if (std::filesystem::is_directory(std::filesystem::current_path(error) / "conker-recomp-data", error)) {
        return;
    }
    std::filesystem::path data;
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg != nullptr && xdg[0] == '/') {
        data = xdg;
    } else if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        data = std::filesystem::path(home) / ".local" / "share";
    } else {
        return;
    }
    const std::filesystem::path folder = data / "ConkersBadFurDayRecompiled";
    std::filesystem::create_directories(folder / "conker-recomp-data", error);
    std::filesystem::current_path(folder, error);
    if (error || !from_desktop()) {
        return;
    }
    const std::string log = (folder / "conker-recomp-data" / "last-run.log").string();
    std::freopen(log.c_str(), "w", stdout);
    // stderr shares stdout's open file, and so its position: a second open of the file kept its own
    // position, and stdout's lines wrote over stderr's (an OUT OF SYNC report was lost that way).
    dup2(fileno(stdout), fileno(stderr));
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
}

}  // namespace conker::launch
