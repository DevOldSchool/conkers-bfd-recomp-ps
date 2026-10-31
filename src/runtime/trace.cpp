#include "runtime/trace.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <unistd.h>

namespace {

constexpr int kRingSize = 64;

struct ThreadTrace {
    const char* ring[kRingSize] = {};
    unsigned head = 0;
    int depth = 0;
};

thread_local ThreadTrace t_trace;

// CONKER_CALL_HASH_LOG=<file>: the game thread's sequence of recompiled function entries, hashed,
// for finding where in a frame two runs of the same input part ways (docs/diagnostics.md). Each
// line is a frame, the number of calls into it and the hash of the sequence so far in the frame:
// one every kCheckEvery calls and one at the end of the frame. CONKER_CALL_HASH_REF=<a log of a
// run that went the other way> compares as the run goes: at the first line that differs, the
// last kNameRing function names are written to <log>.split. CONKER_CALL_DUMP=<frame>:<calls> writes
// the same window of names ending at that point to <log>.dump, to compare with.
constexpr uint32_t kCheckEvery = 4096;
constexpr uint32_t kNameRing = 8192;
const char kAnchor[] = "anchor";  // names are hashed by their offset from this, stable in one binary

struct CallLine {
    uint32_t frame;
    uint32_t calls;
    uint64_t hash;
};

struct CallCheck {
    FILE* log = nullptr;
    std::vector<CallLine> ref;
    size_t ref_at = 0;
    bool split = false;
    uint32_t dump_frame = 0;
    uint32_t dump_calls = 0;
    uint32_t frame = 0;
    uint32_t calls = 0;
    uint64_t hash = 0;
    uint64_t total = 0;
    const char* ring[kNameRing] = {};
};

CallCheck* g_calls = nullptr;
thread_local bool t_game_thread = false;

void write_names(const char* suffix, const char* what) {
    const char* path = std::getenv("CONKER_CALL_HASH_LOG");
    char name[1024];
    std::snprintf(name, sizeof(name), "%s.%s", path, suffix);
    FILE* file = std::fopen(name, "w");
    if (file == nullptr) {
        return;
    }
    CallCheck& c = *g_calls;
    std::fprintf(file, "# %s: frame %u, call %u\n", what, c.frame, c.calls);
    const uint64_t count = c.total < kNameRing ? c.total : kNameRing;
    for (uint64_t i = 0; i < count; i++) {
        const char* n = c.ring[(c.total - count + i) % kNameRing];
        std::fprintf(file, "%s\n", n != nullptr ? n : "?");
    }
    std::fclose(file);
    std::fprintf(stderr, "[conker] call hash: %s at frame %u call %u, names in %s\n", what, c.frame, c.calls, name);
}

void check_line() {
    CallCheck& c = *g_calls;
    std::fprintf(c.log, "%u %u %016llx\n", c.frame, c.calls, static_cast<unsigned long long>(c.hash));
    if (c.split || c.ref.empty()) {
        return;
    }
    const bool same = c.ref_at < c.ref.size() && c.ref[c.ref_at].frame == c.frame && c.ref[c.ref_at].calls == c.calls &&
                      c.ref[c.ref_at].hash == c.hash;
    c.ref_at++;
    if (!same) {
        c.split = true;
        std::fflush(c.log);
        write_names("split", "differs from the reference");
    }
}

void count_call(const char* name) {
    CallCheck& c = *g_calls;
    c.ring[c.total++ % kNameRing] = name;
    c.calls++;
    c.hash = (c.hash ^ static_cast<uint64_t>(name - kAnchor)) * 0x100000001B3ull;
    if (c.calls % kCheckEvery == 0) {
        check_line();
    }
    if (c.frame == c.dump_frame && c.calls == c.dump_calls) {
        write_names("dump", "dump");
    }
}

} // namespace

extern "C" void conker_trace_entry(const char* name) {
    t_trace.ring[t_trace.head++ % kRingSize] = name;
    t_trace.depth++;
    if (t_game_thread) {
        count_call(name);
    }
}

extern "C" void conker_trace_return(const char* /*name*/) {
    if (t_trace.depth > 0) {
        t_trace.depth--;
    }
}

namespace conker::trace {

void game_frame(uint32_t frame) {
    static const bool on = [] {
        const char* path = std::getenv("CONKER_CALL_HASH_LOG");
        if (path == nullptr || path[0] == '\0') {
            return false;
        }
        g_calls = new CallCheck;
        g_calls->log = std::fopen(path, "w");
        if (g_calls->log == nullptr) {
            return false;
        }
        std::setvbuf(g_calls->log, nullptr, _IOFBF, 1 << 20);
        if (const char* ref = std::getenv("CONKER_CALL_HASH_REF")) {
            if (FILE* file = std::fopen(ref, "r")) {
                CallLine line;
                unsigned long long hash = 0;
                while (std::fscanf(file, "%u %u %llx", &line.frame, &line.calls, &hash) == 3) {
                    line.hash = hash;
                    g_calls->ref.push_back(line);
                }
                std::fclose(file);
            }
        }
        if (const char* dump = std::getenv("CONKER_CALL_DUMP")) {
            std::sscanf(dump, "%u:%u", &g_calls->dump_frame, &g_calls->dump_calls);
        }
        return true;
    }();
    if (!on) {
        return;
    }
    t_game_thread = true;
    CallCheck& c = *g_calls;
    if (c.frame != 0) {
        check_line();  // the end of the frame before
    }
    c.frame = frame;
    c.calls = 0;
    c.hash = 0xCBF29CE484222325ull;
    if (frame % 300 == 0) {
        std::fflush(c.log);  // runs end by being killed
    }
}

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
