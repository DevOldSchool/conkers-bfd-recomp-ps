#include "platform/process.hpp"

#include <cstdio>
#include <cstdlib>

#if defined(_WIN32)
#include <windows.h>
#else
#include <climits>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif

namespace conker::process {

void set_env(const char* name, const char* value) {
#if defined(_WIN32)
    _putenv_s(name, value);  // the process environment too, which a started process inherits
#else
    setenv(name, value, 1);
#endif
}

void unset_env(const char* name) {
#if defined(_WIN32)
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

#if defined(_WIN32)
namespace {

std::wstring widen(const std::string& text) {
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), size);
    return wide;
}

// Quotes one argument the way CommandLineToArgvW and the C runtime read it back.
void append_quoted(std::wstring& line, const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        line += arg;
        return;
    }
    line += L'"';
    for (size_t i = 0;; i++) {
        size_t backslashes = 0;
        while (i < arg.size() && arg[i] == L'\\') {
            i++;
            backslashes++;
        }
        if (i == arg.size()) {
            line.append(backslashes * 2, L'\\');
            break;
        }
        if (arg[i] == L'"') {
            line.append(backslashes * 2 + 1, L'\\');
        } else {
            line.append(backslashes, L'\\');
        }
        line += arg[i];
    }
    line += L'"';
}

} // namespace
#endif

void start_again(const std::vector<std::string>& args) {
    std::fflush(nullptr);
#if defined(_WIN32)
    wchar_t exe[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH) == 0) {
        std::fprintf(stderr, "[conker] could not start again: no executable path (Windows error %lu)\n", GetLastError());
        return;
    }
    std::wstring line;
    for (size_t i = 0; i < args.size(); i++) {
        if (i > 0) {
            line += L' ';
        }
        append_quoted(line, i == 0 ? std::wstring(exe) : widen(args[i]));
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION started{};
    if (!CreateProcessW(exe, line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &started)) {
        std::fprintf(stderr, "[conker] could not start again (Windows error %lu)\n", GetLastError());
        return;
    }
    CloseHandle(started.hThread);
    CloseHandle(started.hProcess);
    // As execv would: none of this process's exit work runs.
    TerminateProcess(GetCurrentProcess(), 0);
#else
    std::vector<char*> argv;
    for (const std::string& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);
    // This program by its own path, not by the name it was started with: "./Game" names nothing once a
    // launch has entered its data folder (the restart did nothing then, with only this line in the log).
    char self[PATH_MAX] = {};
#if defined(__APPLE__)
    char raw[PATH_MAX] = {};
    uint32_t raw_size = sizeof(raw);
    if (_NSGetExecutablePath(raw, &raw_size) != 0 || realpath(raw, self) == nullptr) {
        self[0] = '\0';
    }
#else
    if (const ssize_t length = readlink("/proc/self/exe", self, sizeof(self) - 1); length > 0) {
        self[length] = '\0';
    } else {
        self[0] = '\0';
    }
#endif
    execv(self[0] != '\0' ? self : argv[0], argv.data());
    std::perror("[conker] could not start again");
#endif
}

} // namespace conker::process
