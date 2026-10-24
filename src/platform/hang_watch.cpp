#include "platform/hang_watch.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>
#include <thread>

#if defined(__APPLE__)
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace conker::hang_watch {

namespace {

using Clock = std::chrono::steady_clock;

constexpr double kHangSeconds = 3.0;
constexpr int kSampleSeconds = 5;

std::atomic<int64_t> g_last_beat_ms{0};  // 0 until the main loop first runs
std::atomic<int64_t> g_hang_from_ms{0};  // the last beat before a reported hang, 0 if none

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

std::string sample_path() {
    const std::time_t now = std::time(nullptr);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", std::localtime(&now));
    const std::filesystem::path folder = std::filesystem::current_path() / "conker-recomp-data" / "debug_reports";
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    return (folder / ("hang-" + std::string(stamp) + ".txt")).string();
}

// `sample <pid> <seconds> -file <path>`, waited for on this thread.
void sample_threads(const std::string& path) {
#if defined(__APPLE__)
    const std::string pid = std::to_string(getpid());
    const std::string seconds = std::to_string(kSampleSeconds);
    char* argv[] = {const_cast<char*>("/usr/bin/sample"), const_cast<char*>(pid.c_str()),
                    const_cast<char*>(seconds.c_str()), const_cast<char*>("-file"), const_cast<char*>(path.c_str()),
                    nullptr};
    posix_spawn_file_actions_t actions;  // its progress lines would only clutter the game's output
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    pid_t child;
    const int spawned = posix_spawn(&child, argv[0], &actions, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    if (spawned != 0) {
        std::printf("[conker] main thread stuck: could not run sample\n");
        return;
    }
    int status;
    waitpid(child, &status, 0);
    std::printf("[conker] main thread stuck: every thread's stack in %s\n", path.c_str());
#else
    (void)path;
#endif
}

void watch() {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        const int64_t beat = g_last_beat_ms.load();
        if (beat == 0 || g_hang_from_ms.load() == beat || now_ms() - beat < kHangSeconds * 1000) {
            continue;
        }
        g_hang_from_ms.store(beat);
        std::printf("[conker] main thread stuck for %.0f s: the window is not answering\n", kHangSeconds);
        std::fflush(stdout);
        sample_threads(sample_path());
        std::fflush(stdout);
    }
}

} // namespace

void start() {
    std::thread(watch).detach();
}

void beat() {
    const int64_t now = now_ms();
    const int64_t last = g_last_beat_ms.exchange(now);
    if (last != 0 && g_hang_from_ms.load() == last) {
        g_hang_from_ms.store(0);
        std::printf("[conker] main thread running again after %.1f s\n", (now - last) / 1000.0);
    }
}

} // namespace conker::hang_watch
