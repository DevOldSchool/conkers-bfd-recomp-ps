// Checks the game's heap once a frame and reports the first damage it finds. Native helper called
// from the [[patches.hook]] snippet for func_80004250 in config/us.toml.
//
// The heap (func_80003BD0 sets it up at D_800E9D10, up to D_80038098) is one list of blocks in
// address order, each a 12-byte header {next, prev, tag << 24 | size} followed by its data, so
// every block's next is its own address + 0xC + size and points back to it. func_80004250 walks
// the list once a frame to age and free the blocks tagged 2..4; when something writes past the end
// of a block, the next block's header is what it hits, and the walk follows the damaged link out of
// RDRAM (scene 49: a SIGBUS at N64 0x03080200, a block whose next held 0x03080200). Checking the
// list just before that walk reports the damage a frame after it was done, with the blocks around
// it and the bytes either side, and writes RDRAM to conker-recomp-data/heap-<time>.rdram
// (big-endian, as tools/cbfd_dl.py reads it) so what was written there can be looked at.
// CONKER_HEAP_CHECK=0 turns the check off.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <string>

#include "recomp.h"
#include "enhancements/pause_save/resume_point.hpp"

namespace {

inline gpr kseg0(uint32_t address) { return static_cast<gpr>(static_cast<int32_t>(address)); }

constexpr uint32_t kFirstBlock = 0x800380B4;  // D_800380B4: the block at the lowest address
constexpr uint32_t kHeapStart = 0x800E9D10;   // D_800E9D10
constexpr uint32_t kHeapEnd = 0x80038098;     // D_80038098: the address the heap ends at
constexpr uint32_t kLastAllocation = 0x800380C0;  // D_800380C0..D0: the last allocation's arguments
constexpr uint32_t kScene = 0x800BE9F0;
constexpr uint32_t kRdramSize = 0x800000;
constexpr int kBlockLimit = 1 << 20;

const bool g_enabled = [] {
    const char* value = std::getenv("CONKER_HEAP_CHECK");
    return value == nullptr || value[0] != '0';
}();
bool g_reported = false;
uint64_t g_checks = 0;

uint32_t word(uint8_t* rdram, uint32_t address) { return static_cast<uint32_t>(MEM_W(0, kseg0(address))); }

void print_words(uint8_t* rdram, const char* label, uint32_t from, uint32_t count) {
    std::printf("    %s", label);
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t address = from + 4 * i;
        if (i % 8 == 0) {
            std::printf("\n      %08X:", address);
        }
        if (address >= 0x80000000 && address < 0x80000000 + kRdramSize) {
            std::printf(" %08X", word(rdram, address));
        }
    }
    std::printf("\n");
}

void write_rdram(uint8_t* rdram) {
    char stamp[32];
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);
    const std::filesystem::path path = conker::resume::data_folder() / (std::string("heap-") + stamp + ".rdram");
    FILE* file = std::fopen(path.string().c_str(), "wb");
    if (file == nullptr) {
        return;
    }
    for (uint32_t address = 0; address < kRdramSize; address += 4) {
        const uint32_t value = word(rdram, 0x80000000 + address);
        const uint8_t bytes[4] = {static_cast<uint8_t>(value >> 24), static_cast<uint8_t>(value >> 16),
                                  static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value)};
        std::fwrite(bytes, 1, 4, file);
    }
    std::fclose(file);
    std::printf("    RDRAM written to %s\n", path.string().c_str());
}

// What is wrong with `block` (whose list predecessor is `previous`), or nullptr if it looks right.
const char* damage(uint8_t* rdram, uint32_t block, uint32_t previous, uint32_t heap_end) {
    if (block < kHeapStart || block >= heap_end || (block & 3) != 0) {
        return "a link points outside the heap";
    }
    const uint32_t next = word(rdram, block);
    if (word(rdram, block + 4) != previous) {
        return "the block's prev does not point back at the block before it";
    }
    if (next != 0 && next != block + 0xC + (word(rdram, block + 8) & 0xFFFFFF)) {
        return "the block's next is not where its size says it ends";
    }
    return nullptr;
}

void report(uint8_t* rdram, uint32_t block, uint32_t previous, uint32_t before_previous, int index, const char* what) {
    g_reported = true;
    std::printf("[conker] heap damaged (check %llu, scene %d): %s\n", static_cast<unsigned long long>(g_checks),
                static_cast<int>(word(rdram, kScene)), what);
    std::printf("    block %d at %08X; the one before at %08X (tag %u, size 0x%X), and before that %08X\n", index, block,
                previous, previous != 0 ? word(rdram, previous + 8) >> 24 : 0,
                previous != 0 ? word(rdram, previous + 8) & 0xFFFFFF : 0, before_previous);
    std::printf("    last allocation: size 0x%X tag %u alignment %u from %s\n", word(rdram, kLastAllocation),
                word(rdram, kLastAllocation + 4), word(rdram, kLastAllocation + 8),
                word(rdram, kLastAllocation + 12) != 0 ? "the top" : "the bottom");
    if (previous != 0) {
        print_words(rdram, "the header of the block before:", previous, 3);
    }
    if (block >= 0x80000000 && block < 0x80000000 + kRdramSize) {
        print_words(rdram, "the end of the block before, then this block's header {next, prev, tag | size}:", block - 0x40,
                    0x20);
    }
    write_rdram(rdram);
}

}  // namespace

// At the start of func_80004250, which walks the heap once a frame.
extern "C" void conker_heap_check(uint8_t* rdram, recomp_context* /*ctx*/) {
    if (!g_enabled || g_reported) {
        return;
    }
    g_checks++;
    const uint32_t heap_end = word(rdram, kHeapEnd);
    uint32_t before_previous = 0;
    uint32_t previous = 0;
    uint32_t block = word(rdram, kFirstBlock);
    for (int index = 0; block != 0 && index < kBlockLimit; index++) {
        if (const char* what = damage(rdram, block, previous, heap_end)) {
            report(rdram, block, previous, before_previous, index, what);
            return;
        }
        before_previous = previous;
        previous = block;
        block = word(rdram, block);
    }
    if (block != 0) {
        report(rdram, block, previous, before_previous, kBlockLimit, "the list does not end");
    }
}
