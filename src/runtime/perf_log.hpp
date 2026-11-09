#pragma once
// CONKER_PERF_LOG=1: where a frame's time goes, printed once a second per part: how many times it ran,
// its average and its longest, in milliseconds. For finding what holds a slower machine under 30 fps.
//   conker::perf::Timer timer(conker::perf::Part::Hash);  // times until the end of the scope
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace conker::perf {

enum class Part { RendererWait, RendererDraw, RendererTask, ViewPass, NetHash, GameFrame, Count };

inline const char* part_name(Part part) {
    switch (part) {
        case Part::RendererWait: return "renderer waits for game memory";
        case Part::RendererDraw: return "RT64 draws a task";
        case Part::RendererTask: return "renderer task in all";
        case Part::ViewPass: return "my view pass (game thread)";
        case Part::NetHash: return "online hashing (game thread)";
        case Part::GameFrame: return "game frame to frame";
        default: return "?";
    }
}

inline bool enabled() {
    static const bool on = [] {
        const char* value = std::getenv("CONKER_PERF_LOG");
        return value != nullptr && value[0] == '1';
    }();
    return on;
}

struct Totals {
    uint64_t count = 0, total_ns = 0, max_ns = 0;
};

inline std::mutex& totals_mutex() {
    static std::mutex mutex;
    return mutex;
}

inline std::array<Totals, static_cast<size_t>(Part::Count)>& totals() {
    static std::array<Totals, static_cast<size_t>(Part::Count)> parts;
    return parts;
}

inline void add(Part part, uint64_t ns) {
    using Clock = std::chrono::steady_clock;
    static Clock::time_point last_print = Clock::now();
    std::lock_guard lock(totals_mutex());
    Totals& t = totals()[static_cast<size_t>(part)];
    t.count++;
    t.total_ns += ns;
    if (ns > t.max_ns) t.max_ns = ns;
    const Clock::time_point now = Clock::now();
    if (now - last_print < std::chrono::seconds(1)) return;
    last_print = now;
    for (size_t i = 0; i < totals().size(); i++) {
        Totals& p = totals()[i];
        if (p.count == 0) continue;
        std::printf("[conker] perf: %-32s %4llu x  avg %6.2f ms  max %7.2f ms\n", part_name(static_cast<Part>(i)),
                    static_cast<unsigned long long>(p.count), p.total_ns / 1e6 / p.count, p.max_ns / 1e6);
        p = Totals{};
    }
}

class Timer {
public:
    explicit Timer(Part part) : part_(part), on_(enabled()) {
        if (on_) start_ = std::chrono::steady_clock::now();
    }
    ~Timer() {
        if (on_) {
            add(part_, static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                 std::chrono::steady_clock::now() - start_).count()));
        }
    }
    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;

private:
    Part part_;
    bool on_;
    std::chrono::steady_clock::time_point start_;
};

// Time between successive calls (the game's frame to frame).
inline void tick(Part part) {
    if (!enabled()) return;
    static std::chrono::steady_clock::time_point last{};
    const auto now = std::chrono::steady_clock::now();
    if (last != std::chrono::steady_clock::time_point{}) {
        add(part, static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now - last).count()));
    }
    last = now;
}

}  // namespace conker::perf
