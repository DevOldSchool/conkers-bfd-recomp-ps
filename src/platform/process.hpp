// Environment variables and starting the game again in place, on POSIX and Windows.
#pragma once

#include <string>
#include <vector>

namespace conker::process {

void set_env(const char* name, const char* value);
void unset_env(const char* name);

// Replaces this process with the executable running now, started with `args` (args[0] is the
// program name as it was given). Returns only if that failed, after printing why. On Windows, where a
// process cannot be replaced, the new one is started and this one exits.
void start_again(const std::vector<std::string>& args);

} // namespace conker::process
