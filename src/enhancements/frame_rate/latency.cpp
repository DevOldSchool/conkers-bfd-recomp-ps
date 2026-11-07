// CONKER_LATENCY_LOG=<file>: input-to-present latency. The game thread samples the pads at the start
// of a frame (conker_frame_input_begin), builds the frame and queues its graphics task; the scheduler
// later hands the task to RT64, which presents one picture for it, or with frame interpolation
// (CONKER_REFRESH) several, each a step further from the previous frame toward this one. Each frame's
// sample time follows its task in a queue to the renderer, which tags the RT64 workload with it
// (rt64-latency-tag.patch); RT64 reports every picture it presents with that tag.
//
// A line per present: "<present ns> <sample ns> <index> <count> <ms>", ms from the sample to the
// present call (the swap chain adds up to one more refresh before light leaves the screen).
// tools/latency_summary.py sums a log up.
#include "enhancements/frame_rate/latency.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>

namespace {

uint64_t now_ns() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

FILE* log_file() {
    static FILE* file = [] {
        const char* path = std::getenv("CONKER_LATENCY_LOG");
        FILE* opened = path != nullptr && path[0] != '\0' ? std::fopen(path, "w") : nullptr;
        if (opened != nullptr) {
            std::setvbuf(opened, nullptr, _IOLBF, BUFSIZ);
        }
        return opened;
    }();
    return file;
}

std::atomic<uint64_t> g_last_sample{0};
std::mutex g_mutex;
std::deque<uint64_t> g_queued;  // sample times of tasks queued but not yet drawn

}  // namespace

bool conker::latency::enabled() { return log_file() != nullptr; }

void conker::latency::input_sampled() {
    if (enabled()) {
        g_last_sample.store(now_ns(), std::memory_order_relaxed);
    }
}

void conker::latency::task_queued() {
    if (!enabled()) {
        return;
    }
    std::lock_guard lock(g_mutex);
    g_queued.push_back(g_last_sample.load(std::memory_order_relaxed));
    while (g_queued.size() > 8) {  // tasks that never reached the renderer
        g_queued.pop_front();
    }
}

uint64_t conker::latency::take_task_sample() {
    if (!enabled()) {
        return 0;
    }
    std::lock_guard lock(g_mutex);
    if (g_queued.empty()) {
        return 0;
    }
    const uint64_t sample = g_queued.front();
    g_queued.pop_front();
    return sample;
}

namespace {
std::atomic<uint64_t> g_presents{0};
}

uint64_t conker::latency::presents() { return g_presents.load(std::memory_order_relaxed); }

void conker::latency::presented(uint64_t sample, int index, int count) {
    g_presents.fetch_add(1, std::memory_order_relaxed);
    FILE* file = log_file();
    if (file == nullptr || sample == 0) {
        return;
    }
    const uint64_t now = now_ns();
    std::fprintf(file, "%llu %llu %d %d %.3f\n", static_cast<unsigned long long>(now),
                 static_cast<unsigned long long>(sample), index, count, (now - sample) / 1e6);
#if defined(_WIN32)
    std::fflush(file);
#endif
}
