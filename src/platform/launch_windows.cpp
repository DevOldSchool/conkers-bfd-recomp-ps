// The Windows build started from the desktop: data folder and log (see launch.hpp). The ROM dialog
// is in launch_nfd.cpp.
#include "platform/launch.hpp"
#include "platform/crash_log.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
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
    crash_log::start_run(folder / "conker-recomp-data");
    const std::wstring log = (folder / "conker-recomp-data" / "last-run.log").wstring();
    _wfreopen(log.c_str(), L"w", stdout);
    // stderr shares stdout's open file, and so its position (see launch_linux.cpp).
    _dup2(_fileno(stdout), _fileno(stderr));
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
}

void warn_if_emulated() {
#if defined(_M_X64) || defined(__x86_64__)
    // The machine Windows runs on, whatever the process is built for (x64 under emulation on ARM is
    // not WOW64, so this, not IsWow64Process, tells).
    USHORT process_machine = 0;
    USHORT native_machine = 0;
    if (!IsWow64Process2(GetCurrentProcess(), &process_machine, &native_machine) ||
        native_machine != IMAGE_FILE_MACHINE_ARM64) {
        return;
    }
    std::fprintf(stderr, "[conker] this is the x64 build, running under emulation on an ARM64 PC: the "
                         "windows-arm64 build runs faster\n");
    if (!from_desktop()) {
        return;
    }
    // Once per data folder: after that, the log says it.
    const std::filesystem::path marker = std::filesystem::path("conker-recomp-data") / "x64-on-arm-notice-shown";
    std::error_code error;
    if (std::filesystem::exists(marker, error)) {
        return;
    }
    std::ofstream(marker) << "shown\n";
    MessageBoxW(nullptr,
                L"This is the x64 build of Conker's Bad Fur Day Recompiled, and this PC has an ARM processor, "
                L"so Windows is emulating it.\n\n"
                L"It will run, but the ARM64 build runs faster: download the zip whose name ends in "
                L"windows-arm64 from the same release.\n\n"
                L"This message is shown once.",
                L"Conker's Bad Fur Day Recompiled", MB_OK | MB_ICONINFORMATION);
#endif
}

}  // namespace conker::launch
