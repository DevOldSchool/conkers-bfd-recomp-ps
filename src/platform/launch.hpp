// Starting the game from the desktop rather than a terminal: the packaged macOS app
// (tools/package_macos.sh, launch_apple.mm) and the Linux build (tools/linux/build.sh,
// launch_linux.cpp). Opened by a double-click there is no terminal for the log or errors and no ROM
// argument, so the game keeps its data in the user's data folder, writes its log there, asks for
// the ROM the first time and shows errors in a window.
#pragma once

#include <filesystem>

namespace conker::launch {

// True when nobody can read the terminal: a macOS .app bundle, or on Linux no terminal on stderr.
// Errors are then shown in a window.
bool from_desktop();

// Makes the user's data folder the working directory, so conker-recomp-data lives there, and sends
// stdout and stderr to conker-recomp-data/last-run.log when from_desktop().
//   macOS: ~/Library/Application Support/ConkersBadFurDay, inside an .app bundle only.
//   Linux: $XDG_DATA_HOME/ConkersBadFurDay (~/.local/share/ConkersBadFurDay), unless the working
//          directory already has a conker-recomp-data folder (a checkout, or an older run there).
void enter_data_folder();

// Asks for the ROM with an open dialog; empty when it is cancelled. Call on the main thread, on
// macOS after SDL's video is initialised so the panel runs in SDL's application.
std::filesystem::path choose_rom();

}  // namespace conker::launch
