// Keeping a failed run's log for the player to send in. A game started from the desktop writes
// conker-recomp-data/last-run.log (launch.hpp), which the next start would overwrite. A marker file
// says a run is under way; a run that ends in a crash, an abort or a fatal error never removes it, so
// the next start finds it and keeps that run's log in conker-recomp-data/crash-reports, where the
// game tells the player to find it. A run that ended normally is kept as previous-run.log.
#pragma once

#include <filesystem>
#include <optional>

namespace conker::crash_log {

// Called by launch::enter_data_folder() before it opens last-run.log: keeps the last run's log
// (crash-reports/crash-<time>.log after a failed run, else previous-run.log) and marks this run as
// under way. `data` is the conker-recomp-data folder.
void start_run(const std::filesystem::path& data);

// The kept log of the last run, when that run failed.
std::optional<std::filesystem::path> previous_crash();

// main() returned: this run did not crash, whatever its exit status (no ROM chosen is not a crash).
void mark_clean_exit();

// The first lines of the log: build, OS, CPU, and the time, for a report to start from.
void log_system_info();

// Where a crash's extra files go (a Windows minidump), as a path made before any crash, so the crash
// handler need not build one. Empty when the game was not started from the desktop.
const std::filesystem::path& reports_folder();

}  // namespace conker::crash_log
