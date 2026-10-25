// The packaged macOS app (tools/package_macos.sh). Opened from Finder it starts in "/" with no
// terminal and no ROM argument, so it keeps its data in ~/Library/Application Support/ConkerRecomp,
// writes its log there, asks for the ROM the first time and shows errors in a window. Outside an
// .app bundle none of this applies and the runtime keeps using the working directory.
#pragma once

#include <filesystem>

namespace conker::app_bundle {

// True when the executable runs from inside an .app bundle.
bool active();

// Makes ~/Library/Application Support/ConkerRecomp the working directory and sends stdout and
// stderr to conker-recomp-data/last-run.log in it. Does nothing outside an .app bundle.
void enter_data_folder();

// Asks for the ROM with an open panel; empty when the panel is cancelled. Call on the main thread
// after SDL's video is initialised, so the panel runs in SDL's application.
std::filesystem::path choose_rom();

}  // namespace conker::app_bundle
