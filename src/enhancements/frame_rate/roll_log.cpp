// CONKER_ROLL_LOG=<file>:<first VI>:<last VI> counts the calls of the game's two random functions
// (func_150ADA20, the integer roll, and func_150ADA68, the float one) between those retraces, by the
// place that called, and writes them when the last one has passed: a line each of the count, which
// function ("i" or "f"), the caller's address in the game's binary (from its start) and its name.
// Two runs of the same pad script over the same retraces, one at 30 fps and one at 60, show which
// places roll once a frame (twice the count at 60) and which by the time that passed (the same):
// the first kind are what src/enhancements/frame_rate/frame_rate.cpp's chances correct
// (tools/roll_compare.py lists them with the MIPS address of each call).
//
// The functions are reached through the redirect every recompiled function checks for a mod's
// replacement (src/runtime/trace.h), so nothing is counted, and nothing costs, without the variable.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#if !defined(_WIN32)
#include <dlfcn.h>
#include <execinfo.h>
#endif

#include "recomp.h"
#include "runtime/game.hpp"

extern "C" volatile uint32_t recomp_function_redirect_generation;
extern "C" void region_150A3A70(uint8_t* rdram, recomp_context* ctx, uint32_t region_target);
extern "C" void func_150ADA20(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_150ADA68(uint8_t* rdram, recomp_context* ctx);

namespace {

struct RollLog {
    std::string path;
    uint64_t first = 0;
    uint64_t last = 0;
    std::mutex mutex;
    std::unordered_map<const void*, uint64_t> counts[2];  // by the caller's return address
    bool written = false;
};

RollLog* g_log = [] () -> RollLog* {
#if defined(_WIN32)
    return nullptr;
#else
    const char* spec = std::getenv("CONKER_ROLL_LOG");
    if (spec == nullptr || spec[0] == '\0') {
        return nullptr;
    }
    const char* last = std::strrchr(spec, ':');
    const char* first = nullptr;
    for (const char* c = spec; last != nullptr && c < last; c++) {
        if (*c == ':') {
            first = c;  // the colon before the last
        }
    }
    if (first == nullptr) {
        std::fprintf(stderr, "[conker] CONKER_ROLL_LOG needs <file>:<first VI>:<last VI>\n");
        return nullptr;
    }
    auto* log = new RollLog;
    log->path.assign(spec, first);
    log->first = std::strtoull(first + 1, nullptr, 10);
    log->last = std::strtoull(last + 1, nullptr, 10);
    recomp_function_redirect_generation = recomp_function_redirect_generation + 1;  // the functions look again
    std::printf("[conker] counting random rolls by caller from VI %llu to %llu into %s\n",
                static_cast<unsigned long long>(log->first), static_cast<unsigned long long>(log->last), log->path.c_str());
    return log;
#endif
}();

#if !defined(_WIN32)
void write_log(RollLog& log) {
    FILE* file = std::fopen(log.path.c_str(), "w");
    if (file == nullptr) {
        return;
    }
    std::fprintf(file, "# rolls from VI %llu to %llu: count, i|f, caller's offset in the binary, caller\n",
                 static_cast<unsigned long long>(log.first), static_cast<unsigned long long>(log.last));
    for (int kind = 0; kind < 2; kind++) {
        for (const auto& [address, count] : log.counts[kind]) {
            Dl_info info{};
            const bool known = dladdr(address, &info) != 0 && info.dli_fbase != nullptr;
            std::fprintf(file, "%llu %c 0x%llx %s\n", static_cast<unsigned long long>(count), kind == 0 ? 'i' : 'f',
                         static_cast<unsigned long long>(known ? static_cast<const char*>(address) - static_cast<const char*>(info.dli_fbase) : 0),
                         known && info.dli_sname != nullptr ? info.dli_sname : "?");
        }
    }
    std::fclose(file);
    std::printf("[conker] random rolls by caller are in %s\n", log.path.c_str());
}

// The first return address up the stack that is not in the random function's own entry or here.
void note(int kind) {
    RollLog& log = *g_log;
    const uint64_t vi = conker::current_vi();
    if (vi < log.first) {
        return;
    }
    std::lock_guard lock(log.mutex);
    if (vi > log.last) {
        if (!log.written) {
            log.written = true;
            write_log(log);
        }
        return;
    }
    void* frames[5] = {};
    const int depth = backtrace(frames, 5);
    static const void* own[2][2];  // each entry function's start and end, found once
    static bool found = false;
    if (!found) {
        found = true;
        const void* starts[2] = {reinterpret_cast<const void*>(&func_150ADA20), reinterpret_cast<const void*>(&func_150ADA68)};
        for (int i = 0; i < 2; i++) {
            own[i][0] = starts[i];
            own[i][1] = static_cast<const char*>(starts[i]) + 256;  // an entry is a few instructions
        }
    }
    // frames[0] is this function, frames[1] the redirect below (or its caller, if it was inlined here).
    for (int i = 1; i < depth; i++) {
        const void* at = frames[i];
        Dl_info info{};
        // Cheap tests first: inside one of the two entries, or in this file's redirects.
        const bool entry = (at >= own[0][0] && at < own[0][1]) || (at >= own[1][0] && at < own[1][1]);
        if (entry) {
            continue;
        }
        static std::unordered_map<const void*, bool> mine;  // under the lock
        auto known = mine.find(at);
        if (known == mine.end()) {
            const bool here = dladdr(at, &info) != 0 && info.dli_sname != nullptr && std::strstr(info.dli_sname, "roll_redirect") != nullptr;
            known = mine.emplace(at, here).first;
        }
        if (known->second) {
            continue;
        }
        log.counts[kind][at]++;
        return;
    }
}
#endif

}  // namespace

extern "C" {

// In place of func_150ADA20 and func_150ADA68 while the log is on: count, then the function itself.
__attribute__((noinline)) void conker_roll_redirect_int(uint8_t* rdram, recomp_context* ctx) {
#if !defined(_WIN32)
    note(0);
#endif
    ctx->r31 = static_cast<gpr>(static_cast<int32_t>(0xFFFFFFFF));
    region_150A3A70(rdram, ctx, 0x150ADA20);
}

__attribute__((noinline)) void conker_roll_redirect_float(uint8_t* rdram, recomp_context* ctx) {
#if !defined(_WIN32)
    note(1);
#endif
    ctx->r31 = static_cast<gpr>(static_cast<int32_t>(0xFFFFFFFF));
    region_150A3A70(rdram, ctx, 0x150ADA68);
}

// For conker_find_redirect (src/runtime/overlays.cpp): the redirect for a recompiled function's name.
recomp_func_t* conker_roll_log_redirect(const char* name) {
    if (g_log == nullptr) {
        return nullptr;
    }
    if (std::strcmp(name, "func_150ADA20") == 0) {
        return conker_roll_redirect_int;
    }
    if (std::strcmp(name, "func_150ADA68") == 0) {
        return conker_roll_redirect_float;
    }
    return nullptr;
}

}  // extern "C"
