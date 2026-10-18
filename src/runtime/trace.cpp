#include "runtime/trace.h"

#include <cstdio>
#include <cstring>

#include <unistd.h>

namespace {

constexpr int kRingSize = 64;

struct ThreadTrace {
    const char* ring[kRingSize] = {};
    unsigned head = 0;
    int depth = 0;
};

thread_local ThreadTrace t_trace;

} // namespace

extern "C" void conker_trace_entry(const char* name) {
    t_trace.ring[t_trace.head++ % kRingSize] = name;
    t_trace.depth++;
}

extern "C" void conker_trace_return(const char* /*name*/) {
    if (t_trace.depth > 0) {
        t_trace.depth--;
    }
}

namespace conker::trace {

// Async-signal-safe enough for a crash report: only reads this thread's buffer.
void dump_recent_calls(int fd) {
    const ThreadTrace& t = t_trace;
    const unsigned count = t.head < kRingSize ? t.head : kRingSize;
    char buf[160];
    int len = std::snprintf(buf, sizeof(buf), "  last %u recompiled function entries on this thread (oldest first, depth now %d):\n", count, t.depth);
    write(fd, buf, len);
    for (unsigned i = 0; i < count; i++) {
        const char* name = t.ring[(t.head - count + i) % kRingSize];
        len = std::snprintf(buf, sizeof(buf), "    %s\n", name ? name : "?");
        write(fd, buf, len);
    }
}

} // namespace conker::trace
