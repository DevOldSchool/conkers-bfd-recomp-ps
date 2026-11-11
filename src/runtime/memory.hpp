// Address-space plumbing for Conker's TLB layout on top of N64ModernRuntime's rdram reservation.
// See docs/memory-layout.md.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace conker::memory {

constexpr uint32_t kMainAliasBase = 0x10000000;   // TLB alias of all of RDRAM, used for main code
constexpr uint32_t kUncachedBase = 0xA0000000;    // KSEG1 view of RDRAM (Rare uses uncached pointers)
constexpr uint32_t kGameCodeVram = 0x15000000;    // game overlay code + rodata image
constexpr uint32_t kGameDataVram = 0x80082B20;    // game overlay data image (ordinary KSEG0)
constexpr uint32_t kDebuggerVram = 0x16000000;    // debugger overlay
constexpr size_t kRdramSize = 8 * 1024 * 1024;    // what the alias has to mirror

constexpr uint32_t kBssStart = 0x8002D4B0;        // cleared by the ROM entry stub
constexpr uint32_t kBssSize = 0x16690;
constexpr uint32_t kInitialSp = 0x800314B0;       // set by the ROM entry stub
constexpr uint32_t kFirstCFunction = 0x10001050;  // func_80001050, reached from the TLB setup stub

constexpr uint32_t kGameArchiveRom = 0x42450;     // compressed game code/data archive
constexpr uint32_t kGameArchiveEnd = 0x19EA88;
constexpr uint32_t kDebuggerRom = 0x19EA88;
constexpr uint32_t kDebuggerSize = 0x4960;

// Offset of an N64 virtual address inside the rdram allocation, exactly as N64Recomp's MEM_*
// macros compute it (sign-extended address minus 0xFFFFFFFF80000000).
inline size_t rdram_offset(uint32_t address) {
    return static_cast<size_t>(static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(address))) - 0xFFFFFFFF80000000ull);
}

// The rdram base, recorded when the alias is installed (for diagnostics).
const uint8_t* current_rdram();

// Makes the 0x10000000 window a true alias of the first `kRdramSize` bytes of RDRAM by backing
// both ranges with the same shared memory. Preserves the current RDRAM contents.
// Returns false (after printing why) if the platform does not support it yet.
bool install_rdram_alias(uint8_t* rdram);

// Makes the window for `vram`..`vram + size` readable and writable. The runtime reserves 4 GiB
// of address space but only commits the first 512 MiB.
bool make_window_accessible(uint8_t* rdram, uint32_t vram, size_t size);

// Memory the renderer can draw a frame from in place of RDRAM (the online "my view"): the first
// kDrawImageSize bytes readable and writable, zero at first, at the start of a reservation shaped as
// RDRAM's own, 4 GiB with nothing else in it and no access past those bytes. RT64 takes a display
// list's addresses as offsets from the start of the memory it draws from, up to 16 MiB of them and
// unchecked, and a list can be another machine's (a host's snapshot): in RDRAM any such offset is
// RDRAM's own reservation, and so it must be here. A buffer of RDRAM's 8 MiB from the heap had other
// allocations right after it, which such a list read and, as a colour image, wrote.
// kDrawImageSize covers the 16 MiB of offsets and a frame buffer written at the last of them.
// Never freed. nullptr (after printing why) if the system gives none.
constexpr size_t kDrawImageSize = 32 * 1024 * 1024;
uint8_t* reserve_draw_image();

// Copies a big-endian byte image into rdram at `vram`, honouring the runtime's word-swapped
// byte layout (the same thing a PI DMA from ROM does).
void write_image(uint8_t* rdram, uint32_t vram, std::span<const uint8_t> image);

} // namespace conker::memory
