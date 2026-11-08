// The PS5 title's start-up (docs/building-ps5.md): its log, crash reports and the system calls the
// console wants before the game runs, and where the player's ROM is found. src/main.cpp calls these.
#pragma once

#include <filesystem>

namespace conker::ps5 {

// The title folder, mounted here while the game runs. The data folder is inside it
// (src/platform/data_folder.hpp).
inline const std::filesystem::path kTitleFolder = "/app0";

// First thing in main, before SDL starts: the log (conker.log in the title folder, which stdout and
// stderr go to unbuffered), crash handlers, a memory report, the imports the console lacks, SDL's
// video driver, and the system's splash screen hidden.
void init();

// Writes out anything buffered in the log.
void flush_log();

// The ROM the player put in the title folder: its first regular file whose extension is .z64, .n64
// or .v64 (lower case). Empty when there is none.
std::filesystem::path find_rom();

}  // namespace conker::ps5
