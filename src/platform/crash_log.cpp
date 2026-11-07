#include "platform/crash_log.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "build_version.h"

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/utsname.h>
#else
#include <sys/utsname.h>
#endif

namespace conker::crash_log {

namespace {

constexpr const char* kMarker = "run-in-progress";
constexpr int kKeptCrashLogs = 10;

std::filesystem::path g_data;
std::filesystem::path g_reports;
std::optional<std::filesystem::path> g_previous_crash;

std::string stamp(std::filesystem::file_time_type when) {
    // The log's last write, the moment the failed run stopped writing, in local time.
    const auto system = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        when - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
    const std::time_t t = std::chrono::system_clock::to_time_t(system);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    char text[32];
    std::strftime(text, sizeof(text), "%Y-%m-%d_%H-%M-%S", &local);
    return text;
}

void prune(const std::filesystem::path& folder) {
    std::error_code error;
    std::vector<std::filesystem::path> logs;
    for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
        if (entry.path().extension() == ".log") {
            logs.push_back(entry.path());
        }
    }
    if (static_cast<int>(logs.size()) <= kKeptCrashLogs) {
        return;
    }
    // Named by time, so the oldest sort first; a minidump of the same name goes with its log.
    std::sort(logs.begin(), logs.end());
    for (size_t i = 0; i + kKeptCrashLogs < logs.size(); i++) {
        std::filesystem::remove(logs[i], error);
        std::filesystem::path dump = logs[i];
        std::filesystem::remove(dump.replace_extension(".dmp"), error);
    }
}

std::string os_description() {
#if defined(_WIN32)
    // GetVersionEx reports what the manifest allows; RtlGetVersion the real version.
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
        if (auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"))) {
            fn(&info);
        }
    }
    std::string text = "Windows " + std::to_string(info.dwMajorVersion) + "." + std::to_string(info.dwMinorVersion) +
                       " build " + std::to_string(info.dwBuildNumber);
    USHORT process_machine = 0;
    USHORT native_machine = 0;
    if (IsWow64Process2(GetCurrentProcess(), &process_machine, &native_machine)) {
        text += native_machine == IMAGE_FILE_MACHINE_ARM64 ? ", ARM64 PC" : ", x64 PC";
    }
    return text;
#elif defined(__APPLE__)
    char version[64] = {};
    size_t size = sizeof(version);
    sysctlbyname("kern.osproductversion", version, &size, nullptr, 0);
    char model[64] = {};
    size = sizeof(model);
    sysctlbyname("hw.model", model, &size, nullptr, 0);
    return std::string("macOS ") + version + ", " + model;
#else
    std::string name = "Linux";
    std::ifstream release("/etc/os-release");
    for (std::string line; std::getline(release, line);) {
        if (line.rfind("PRETTY_NAME=", 0) == 0) {
            name = line.substr(12);
            name.erase(std::remove(name.begin(), name.end(), '"'), name.end());
            break;
        }
    }
    utsname uts{};
    if (uname(&uts) == 0) {
        name += std::string(", kernel ") + uts.release;
    }
    return name;
#endif
}

const char* cpu() {
#if defined(_M_ARM64) || defined(__aarch64__)
    return "arm64";
#elif defined(_M_X64) || defined(__x86_64__)
    return "x86-64";
#else
    return "unknown";
#endif
}

}  // namespace

void start_run(const std::filesystem::path& data) {
    g_data = data;
    g_reports = data / "crash-reports";
    std::error_code error;
    const std::filesystem::path log = data / "last-run.log";
    const bool failed = std::filesystem::exists(data / kMarker, error);
    if (std::filesystem::exists(log, error)) {
        if (failed) {
            std::filesystem::create_directories(g_reports, error);
            const auto when = std::filesystem::last_write_time(log, error);
            // Two failed runs in the same second (one that failed at once, then its retry) get -2, -3.
            const std::string name = "crash-" + stamp(when);
            std::filesystem::path kept = g_reports / (name + ".log");
            for (int n = 2; std::filesystem::exists(kept, error) && n < 100; n++) {
                kept = g_reports / (name + "-" + std::to_string(n) + ".log");
            }
            std::filesystem::rename(log, kept, error);
            if (!error) {
                g_previous_crash = kept;
                // A Windows minidump from that crash, written under the name "pending" (main.cpp).
                std::filesystem::path dump = kept;
                std::filesystem::rename(g_reports / "pending.dmp", dump.replace_extension(".dmp"), error);
            }
            prune(g_reports);
        } else {
            std::filesystem::rename(log, data / "previous-run.log", error);
        }
    }
    std::ofstream(data / kMarker) << "The game is running, or the last run did not end normally.\n";
}

std::optional<std::filesystem::path> previous_crash() {
    return g_previous_crash;
}

void mark_clean_exit() {
    if (g_data.empty()) {
        return;
    }
    std::error_code error;
    std::filesystem::remove(g_data / kMarker, error);
}

void log_system_info() {
    const std::time_t now = std::time(nullptr);
    char when[32];
    std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    std::printf("[conker] Conker's Bad Fur Day Recompiled %s (%s), started %s\n", CONKER_BUILD_VERSION, cpu(), when);
    std::printf("[conker] %s, %u CPU threads\n", os_description().c_str(), std::thread::hardware_concurrency());
    if (g_previous_crash) {
        // Relative to the data folder: the full path names the player's account, and logs get posted.
        std::printf("[conker] the last run did not end normally; its log is crash-reports/%s\n",
                    g_previous_crash->filename().string().c_str());
    }
}

const std::filesystem::path& reports_folder() {
    return g_reports;
}

}  // namespace conker::crash_log
