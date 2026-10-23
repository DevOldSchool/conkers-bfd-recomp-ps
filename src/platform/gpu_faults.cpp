// GPU faults. On macOS a command buffer that faults on the GPU (a read or write outside any
// resource, "GPU Address Fault Error") is thrown away, so its frame is lost: the picture stutters
// or flashes. After a handful of them macOS stops running the process's GPU work altogether
// ("Ignored (for causing prior/excessive GPU errors)") and the window stays black until the game is
// restarted, though the game, its sound and its menus carry on. RT64 never looked at whether a
// command buffer succeeded, so the only trace was in the system log.
//
// Plume's Metal back end now reports failed command buffers (tools/patches/plume-gpu-errors.patch),
// with the RT64 worker whose queue it was sent on (tools/patches/rt64-queue-names.patch) and the
// encoders that faulted. Each fault is printed with the VI, the time, the scene and the renderer's
// recent resource changes (note), which are the usual suspects for a GPU reading freed memory. When
// macOS starts refusing the work, that is said once, and again every 30 s while it lasts, as the
// picture will not come back. Work dropped when another program's fault resets the GPU ("innocent
// victim") gets a line of its own and is not counted. The first fault also asks for a debug report
// (src/enhancements/debug_menu/debug_menu.cpp), which keeps these lines in report.json.
#include "platform/gpu_faults.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>
#include <vector>

#include "runtime/game.hpp"

namespace conker::gpu_faults {

namespace {

// IOGPU's code for a command buffer refused because of earlier faults
// (kIOGPUCommandBufferCallbackErrorSubmissionsIgnored).
constexpr int64_t kSubmissionsIgnored = 4;
constexpr uint32_t kScene = 0x800BE9F0;  // D_800BE9F0
constexpr size_t kNotesKept = 12;
constexpr size_t kFaultsKept = 16;
constexpr auto kRefusedRepeat = std::chrono::seconds(30);

using Clock = std::chrono::steady_clock;

const uint8_t* g_rdram = nullptr;
const Clock::time_point g_started = Clock::now();
std::mutex g_mutex;
std::deque<std::string> g_notes;   // recent renderer events, not yet printed with a fault
std::deque<std::string> g_faults;  // the faults' lines, for status()
uint64_t g_fault_count = 0;
uint64_t g_refused_count = 0;
uint64_t g_refused_since_print = 0;
Clock::time_point g_refused_printed;
std::atomic<bool> g_report_wanted{false};

// "VI <n> (<s> s, <local time>)"
std::string when() {
    const double seconds = std::chrono::duration<double>(Clock::now() - g_started).count();
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char clock[16];
    std::strftime(clock, sizeof(clock), "%H:%M:%S", &local);
    char text[96];
    std::snprintf(text, sizeof(text), "VI %llu (%.1f s, %s)", static_cast<unsigned long long>(conker::current_vi()),
                  seconds, clock);
    return text;
}

int scene() {
    if (g_rdram == nullptr) {
        return -1;
    }
    // RDRAM holds native-endian words.
    return *reinterpret_cast<const int32_t*>(g_rdram + (kScene - 0x80000000u));
}

}  // namespace

void start(const uint8_t* rdram) { g_rdram = rdram; }

void note(const std::string& event) {
    std::lock_guard lock(g_mutex);
    g_notes.push_back(when() + ": " + event);
    if (g_notes.size() > kNotesKept) {
        g_notes.pop_front();
    }
}

void report(const char* queue, int64_t code, const char* description, const char* encoders) {
    std::lock_guard lock(g_mutex);
    if (code == kSubmissionsIgnored) {
        g_refused_count++;
        g_refused_since_print++;
        const Clock::time_point now = Clock::now();
        if (g_refused_count == 1 || now - g_refused_printed >= kRefusedRepeat) {
            std::printf("[conker] GPU: macOS is refusing the game's GPU work after %llu GPU fault(s) (%llu command "
                         "buffers refused, %llu since the last line), so the picture stays black until the game is "
                         "restarted; the faults are above. %s\n",
                         static_cast<unsigned long long>(g_fault_count),
                         static_cast<unsigned long long>(g_refused_count),
                         static_cast<unsigned long long>(g_refused_since_print), when().c_str());
            g_refused_printed = now;
            g_refused_since_print = 0;
        }
        return;
    }
    if (std::strstr(description, "Victim") != nullptr || std::strstr(description, "victim") != nullptr) {
        // The GPU was reset for another process's fault and this game's work in flight was dropped.
        std::printf("[conker] GPU: a frame was dropped in a GPU reset caused by another program (%s queue), not a "
                    "fault of the game's. %s\n", queue, when().c_str());
        return;
    }
    g_fault_count++;
    char line[1024];
    std::snprintf(line, sizeof(line), "GPU fault %llu at %s, scene %d: %s queue, code %lld, faulted encoders: %s: %s",
                  static_cast<unsigned long long>(g_fault_count), when().c_str(), scene(), queue,
                  static_cast<long long>(code), encoders, description);
    std::printf("[conker] %s\n", line);
    if (g_notes.empty()) {
        std::printf("[conker]   no renderer changes since the last fault\n");
    } else {
        std::printf("[conker]   renderer changes before it (since the last fault):\n");
        for (const std::string& event : g_notes) {
            std::printf("[conker]     %s\n", event.c_str());
        }
    }
    g_faults.push_back(line);
    for (const std::string& event : g_notes) {
        g_faults.push_back("  before it: " + event);
    }
    while (g_faults.size() > kFaultsKept) {
        g_faults.pop_front();
    }
    g_notes.clear();
    if (g_fault_count == 1) {
        g_report_wanted.store(true);
    }
}

nlohmann::json status() {
    std::lock_guard lock(g_mutex);
    return {{"faults", g_fault_count},
            {"refused_command_buffers", g_refused_count},
            {"log", nlohmann::json(std::vector<std::string>(g_faults.begin(), g_faults.end()))},
            {"recent_renderer_changes", nlohmann::json(std::vector<std::string>(g_notes.begin(), g_notes.end()))}};
}

bool take_report_request() { return g_report_wanted.exchange(false); }

void request_report_again() { g_report_wanted.store(true); }

} // namespace conker::gpu_faults
