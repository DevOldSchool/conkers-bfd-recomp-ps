// Starting the game from the desktop rather than a terminal: the packaged macOS app
// (tools/package_macos.sh, launch_apple.mm), the Linux build (tools/linux/build.sh,
// launch_linux.cpp) and the Windows build (launch_windows.cpp). Opened by a double-click there is no terminal for the log or errors and no ROM
// argument, so the game keeps its data in the user's data folder, writes its log there, asks for
// the ROM the first time and shows errors in a window.
#pragma once

#include <filesystem>

namespace conker::launch {

// True when nobody can read the terminal: a macOS .app bundle, on Linux no terminal on stderr, on
// Windows a console opened for the game alone (a double-click in Explorer).
// Errors are then shown in a window.
bool from_desktop();

// Makes the user's data folder the working directory, so conker-recomp-data lives there, and sends
// stdout and stderr to conker-recomp-data/last-run.log when from_desktop().
//   macOS: ~/Library/Application Support/ConkersBadFurDayRecompiled, inside an .app bundle only.
//   Linux: $XDG_DATA_HOME/ConkersBadFurDayRecompiled (~/.local/share/ConkersBadFurDayRecompiled),
//          unless the working directory already has a conker-recomp-data folder (a checkout, or an
//          older run there).
//   Windows: %LOCALAPPDATA%\ConkersBadFurDayRecompiled, with the same exception as Linux.
void enter_data_folder();

// Windows only: when the x64 build runs under emulation on an ARM PC, says that the ARM64 build
// runs faster, in the log every time and in a window the first time (or every time from a terminal,
// only in the log). Call after enter_data_folder(). Elsewhere it does nothing.
void warn_if_emulated();

// Asks for the ROM with an open dialog; empty when it is cancelled. Call on the main thread, on
// macOS after SDL's video is initialised so the panel runs in SDL's application.
std::filesystem::path choose_rom();

}  // namespace conker::launch
