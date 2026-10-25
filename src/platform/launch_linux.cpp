// The Linux build started from the desktop: data folder, log and ROM dialog (see launch.hpp). The
// dialog is nativefiledialog-extended's GTK one, which RT64 already builds.
#include "platform/launch.hpp"

#include <cstdio>
#include <cstdlib>
#include <system_error>

#include <unistd.h>

#include "nfd.h"

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
    const std::filesystem::path folder = data / "ConkersBadFurDay";
    std::filesystem::create_directories(folder / "conker-recomp-data", error);
    std::filesystem::current_path(folder, error);
    if (error || !from_desktop()) {
        return;
    }
    const std::string log = (folder / "conker-recomp-data" / "last-run.log").string();
    std::freopen(log.c_str(), "w", stdout);
    std::freopen(log.c_str(), "a", stderr);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
}

std::filesystem::path choose_rom() {
    if (NFD_Init() != NFD_OKAY) {
        std::fprintf(stderr, "[conker] no file dialog: %s\n", NFD_GetError());
        return {};
    }
    const nfdu8filteritem_t filter{"Nintendo 64 ROM", "z64,n64,v64"};
    nfdu8char_t* path = nullptr;
    std::filesystem::path rom;
    if (NFD_OpenDialogU8(&path, &filter, 1, nullptr) == NFD_OKAY) {
        rom = path;
        NFD_FreePathU8(path);
    }
    NFD_Quit();
    return rom;
}

}  // namespace conker::launch
