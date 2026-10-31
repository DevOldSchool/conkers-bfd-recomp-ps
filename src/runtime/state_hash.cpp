// Per-frame hashes of RDRAM; see state_hash.hpp.
#include "runtime/state_hash.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <execinfo.h>
#include <vector>

#include "recomp.h"

namespace {

constexpr uint32_t kStart = 0x00000000;  // physical: 0x80000000
constexpr uint32_t kEnd = 0x00400000;    // the 4 MiB the game uses
constexpr uint32_t kBlock = 0x4000;
constexpr int kBlocks = (kEnd - kStart) / kBlock;
// Left out of the hashes: words that hold when things happened rather than what the game did.
struct Masked {
    uint32_t start;  // physical
    uint32_t size;
};
constexpr Masked kMasked[] = {
    {0x000BE6E8, 0x10},  // the interval and value of the OSTimer the game sets at 0x800BE6E0: clock readings
    // The game thread's message queue (D_800BEA10, its count and its 16 slots): whether the
    // scheduler's next message is already waiting when a frame starts. The messages are taken in
    // the same order either way.
    {0x000BEA18, 0x04},
    {0x000BEA28, 0x40},
};

const int g_every = [] {
    const char* value = std::getenv("CONKER_STATE_HASH_EVERY");
    const int every = value != nullptr ? std::atoi(value) : 1;
    return every > 0 ? every : 1;
}();

FILE* log_file() {
    static FILE* file = [] {
        const char* path = std::getenv("CONKER_STATE_HASH_LOG");
        FILE* opened = path != nullptr && path[0] != '\0' ? std::fopen(path, "w") : nullptr;
        if (opened != nullptr) {
            std::setvbuf(opened, nullptr, _IOFBF, 1 << 20);
        }
        return opened;
    }();
    return file;
}

// A 64-bit multiply-xorshift over the block's words, folded to 32 bits. Not cryptographic; it only
// has to tell blocks apart.
uint32_t hash_block(const uint8_t* data) {
    uint64_t hash = 0x9E3779B97F4A7C15ull;
    for (uint32_t i = 0; i < kBlock; i += 8) {
        uint64_t word;
        std::memcpy(&word, data + i, sizeof(word));
        hash = (hash ^ word) * 0xFF51AFD7ED558CCDull;
        hash ^= hash >> 32;
    }
    return static_cast<uint32_t>(hash ^ (hash >> 32));
}

// CONKER_STATE_DUMP_FRAMES=<n>[,<n>...] with CONKER_STATE_DUMP_DIR=<dir>: the 4 MiB as
// <dir>/frame<n>.bin at those frames, as the runtime holds it (each 32-bit word in host order).
void dump(uint8_t* rdram, uint32_t frame) {
    static const char* frames = std::getenv("CONKER_STATE_DUMP_FRAMES");
    static const char* dir = std::getenv("CONKER_STATE_DUMP_DIR");
    if (frames == nullptr || dir == nullptr) {
        return;
    }
    for (const char* at = frames; *at != '\0';) {
        char* next = nullptr;
        const unsigned long listed = std::strtoul(at, &next, 10);
        if (next == at) {
            break;
        }
        if (listed == frame) {
            char path[1024];
            std::snprintf(path, sizeof(path), "%s/frame%u.bin", dir, frame);
            if (FILE* file = std::fopen(path, "wb")) {
                std::fwrite(rdram + kStart, 1, kEnd - kStart, file);
                std::fclose(file);
            }
            return;
        }
        at = *next == ',' ? next + 1 : next;
    }
}

// CONKER_STATE_WATCH=<start>-<end>[,...] with CONKER_STATE_WATCH_LOG=<file>: those ranges of RDRAM
// (physical or KSEG0 addresses) appended to the file every frame, as the runtime holds them, after
// the frame number, for finding the bytes behind a block that differs now and then.
void watch(uint8_t* rdram, uint32_t frame) {
    struct Range {
        uint32_t start;
        uint32_t end;
    };
    static std::vector<Range> ranges;
    static FILE* file = [] {
        const char* list = std::getenv("CONKER_STATE_WATCH");
        const char* path = std::getenv("CONKER_STATE_WATCH_LOG");
        if (list == nullptr || path == nullptr) {
            return static_cast<FILE*>(nullptr);
        }
        for (const char* at = list; *at != '\0';) {
            char* next = nullptr;
            const uint32_t start = static_cast<uint32_t>(std::strtoul(at, &next, 0)) & 0x00FFFFFF;
            if (next == at || *next != '-') {
                break;
            }
            at = next + 1;
            const uint32_t end = static_cast<uint32_t>(std::strtoul(at, &next, 0)) & 0x00FFFFFF;
            if (next == at) {
                break;
            }
            if (start < end && end <= kEnd) {
                ranges.push_back({start & ~3u, (end + 3) & ~3u});
            }
            at = *next == ',' ? next + 1 : next;
        }
        FILE* opened = ranges.empty() ? nullptr : std::fopen(path, "wb");
        if (opened != nullptr) {
            std::setvbuf(opened, nullptr, _IOFBF, 1 << 20);
        }
        return opened;
    }();
    if (file == nullptr) {
        return;
    }
    std::fwrite(&frame, sizeof(frame), 1, file);
    for (const Range& range : ranges) {
        std::fwrite(rdram + range.start, 1, range.end - range.start, file);
    }
    if (frame % 300 == 0) {
        std::fflush(file);
    }
}

}  // namespace

namespace {
uint32_t g_frame = 0;
}

uint32_t conker::state_hash::current_frame() { return g_frame; }

void conker::state_hash::frame(uint8_t* live_rdram, uint32_t frame) {
    g_frame = frame;
    FILE* file = log_file();
    const bool hashing = file != nullptr && frame % g_every == 0;
    static const bool copying = std::getenv("CONKER_STATE_DUMP_FRAMES") != nullptr || std::getenv("CONKER_STATE_WATCH") != nullptr;
    if (!hashing && !copying) {
        return;
    }
    // One copy for the dump, the watch and the hashes, so that they agree with each other even where
    // another thread writes RDRAM while they are taken.
    static uint8_t* snapshot = new uint8_t[kEnd - kStart];
    std::memcpy(snapshot, live_rdram + kStart, kEnd - kStart);
    uint8_t* rdram = snapshot - kStart;
    dump(rdram, frame);
    watch(rdram, frame);
    if (!hashing) {
        return;
    }
    std::fprintf(file, "%u", frame);
    for (int block = 0; block < kBlocks; block++) {
        const uint32_t block_start = kStart + block * kBlock;
        const uint8_t* data = rdram + block_start;
        static uint8_t copy[kBlock];
        bool copied = false;
        for (const Masked& masked : kMasked) {
            if (masked.start >= block_start && masked.start < block_start + kBlock) {
                if (!copied) {
                    std::memcpy(copy, data, kBlock);
                    copied = true;
                }
                std::memset(copy + (masked.start - block_start), 0, masked.size);
            }
        }
        if (copied) {
            data = copy;
        }
        std::fprintf(file, " %08x", hash_block(data));
    }
    std::fputc('\n', file);
    if (frame % 300 == 0) {
        std::fflush(file);  // runs end by being killed
    }
}

// CONKER_ALLOC_LOG=<file>: a line for each heap allocation and free the game makes (func_80003C6C,
// __conker_mp3_free), with the frame, the size or pointer and the recompiled functions that called it,
// for finding the first one two runs disagree on. Game thread only, so the frame is the one running.
extern "C" void conker_alloc_log(uint8_t* /*rdram*/, recomp_context* ctx, int is_free) {
    static FILE* file = [] {
        const char* path = std::getenv("CONKER_ALLOC_LOG");
        FILE* opened = path != nullptr && path[0] != '\0' ? std::fopen(path, "w") : nullptr;
        if (opened != nullptr) {
            std::setvbuf(opened, nullptr, _IOFBF, 1 << 20);
        }
        return opened;
    }();
    if (file == nullptr) {
        return;
    }
    std::fprintf(file, "%u %s %08x %x", conker::state_hash::current_frame(), is_free ? "free" : "alloc",
                 static_cast<uint32_t>(ctx->r4), static_cast<uint32_t>(ctx->r5));
    void* frames[8];
    const int count = backtrace(frames, 8);
    for (int i = 1; i < count; i++) {
        Dl_info info;
        if (dladdr(frames[i], &info) != 0 && info.dli_sname != nullptr && std::strncmp(info.dli_sname, "func_", 5) == 0) {
            std::fprintf(file, " %s", info.dli_sname);
        }
    }
    std::fputc('\n', file);
}
