// The Windows build started from the desktop: data folder and log (see launch.hpp). The ROM dialog
// is in launch_nfd.cpp.
#include "platform/launch.hpp"

#include <cstdio>
#include <cstdlib>
#include <io.h>
#include <system_error>

#include <windows.h>

namespace conker::launch {

namespace {

// A console of its own, made for this process when it was opened from Explorer, has no other
// process attached. Read once, before enter_data_folder lets that console go.
const bool g_from_desktop = [] {
    DWORD processes[2];
    return GetConsoleProcessList(processes, 2) <= 1;
}();

}  // namespace

bool from_desktop() {
    return g_from_desktop;
}

void enter_data_folder() {
    std::error_code error;
    if (std::filesystem::is_directory(std::filesystem::current_path(error) / "conker-recomp-data", error)) {
        return;
    }
    const wchar_t* local = _wgetenv(L"LOCALAPPDATA");
    if (local == nullptr || local[0] == L'\0') {
        return;
    }
    const std::filesystem::path folder = std::filesystem::path(local) / "ConkersBadFurDayRecompiled";
    std::filesystem::create_directories(folder / "conker-recomp-data", error);
    std::filesystem::current_path(folder, error);
    if (error || !from_desktop()) {
        return;
    }
    // Nobody reads the console Windows opened for the game: close it, and log to a file instead.
    FreeConsole();
    const std::wstring log = (folder / "conker-recomp-data" / "last-run.log").wstring();
    _wfreopen(log.c_str(), L"w", stdout);
    // stderr shares stdout's open file, and so its position (see launch_linux.cpp).
    _dup2(_fileno(stdout), _fileno(stderr));
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
}

}  // namespace conker::launch
