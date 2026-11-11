#include "runtime/memory.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

#if CONKER_PS5
#include <ps5platform/kernel.h>

#include "librecomp/addresses.hpp"
#elif defined(CONKER_PLATFORM_POSIX)
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#elif defined(_WIN32)
#include <windows.h>

#include "librecomp/addresses.hpp"
#endif

namespace conker::memory {

namespace {

size_t page_size() {
#if CONKER_PS5
    // Direct memory is mapped in its own 64 KiB units, larger than the kernel's pages.
    return PS5_KERNEL_DIRECT_ALIGNMENT;
#elif defined(CONKER_PLATFORM_POSIX)
    return static_cast<size_t>(sysconf(_SC_PAGESIZE));
#else
    return 4096;
#endif
}

#if CONKER_PS5
// The console has no shared-memory objects to map at several addresses, but one direct-memory
// allocation can be mapped at any number of them: that is the alias. librecomp backs rdram's first
// 512 MiB with direct memory of its own (tools/patches/n64modernruntime-ps5.patch) and keeps the
// rest of its 4 GiB reserved, so each mapping below replaces part of that reservation.

void print_kernel_error(const char* what, int32_t result) {
    std::fprintf(stderr, "[conker] %s failed (0x%08X)\n", what, static_cast<unsigned>(result));
}

// Maps `size` bytes of direct memory from `direct` at exactly `address`, readable and writable.
bool map_direct_at(uint8_t* address, size_t size, int64_t direct, const char* what) {
    void* mapped = address;
    const int32_t result = sceKernelMapDirectMemory(&mapped, size, PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE,
                                                    PS5_KERNEL_MAP_FIXED, direct, PS5_KERNEL_DIRECT_ALIGNMENT);
    if (result != 0 || mapped != address) {
        print_kernel_error(what, result);
        return false;
    }
    return true;
}

bool allocate_direct(size_t size, int64_t* direct, const char* what) {
    const int32_t result = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), size, PS5_KERNEL_DIRECT_ALIGNMENT,
                                                         PS5_KERNEL_DIRECT_TYPE_CPU, direct);
    if (result != 0) {
        print_kernel_error(what, result);
        return false;
    }
    return true;
}

// The 64 KiB units, as offsets into the reservation, that already have memory behind them: rdram's
// first 512 MiB (librecomp's), the two aliases, and every window made accessible so far. A window
// sharing a unit with one before it must not replace that unit's memory.
std::vector<std::pair<size_t, size_t>> g_backed;

bool is_backed(size_t offset) {
    for (const auto& [start, end] : g_backed) {
        if (offset >= start && offset < end) {
            return true;
        }
    }
    return false;
}
#endif

#if defined(_WIN32)
// Windows cannot map a view over memory that is already reserved, so the 4 GiB librecomp reserved
// is released and reserved again as a placeholder, which can be split and have views mapped into
// its pieces (Windows 10 1803 and later). Everything outside the views is reserved as before, with
// the first 512 MiB readable and writable; only those were committed, but the windows above 512 MiB
// are committed one by one in make_window_accessible.
using VirtualAlloc2Fn = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
using MapViewOfFile3Fn = PVOID(WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);

void print_windows_error(const char* what) {
    std::fprintf(stderr, "[conker] %s failed (Windows error %lu)\n", what, GetLastError());
}

bool replace_rdram_reservation(uint8_t* rdram) {
    HMODULE kernelbase = GetModuleHandleW(L"kernelbase.dll");
    auto virtual_alloc2 = reinterpret_cast<VirtualAlloc2Fn>(
        reinterpret_cast<void*>(GetProcAddress(kernelbase, "VirtualAlloc2")));
    auto map_view_of_file3 = reinterpret_cast<MapViewOfFile3Fn>(
        reinterpret_cast<void*>(GetProcAddress(kernelbase, "MapViewOfFile3")));
    if (virtual_alloc2 == nullptr || map_view_of_file3 == nullptr) {
        std::fprintf(stderr, "[conker] RDRAM aliasing needs Windows 10 version 1803 or later\n");
        return false;
    }
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                        static_cast<DWORD>(kRdramSize), nullptr);
    if (section == nullptr) {
        print_windows_error("CreateFileMapping (rdram alias)");
        return false;
    }

    // Offsets from the start of librecomp's reservation, which has a guard before rdram.
    uint8_t* const base = rdram - recomp::rdram_guard_size;
    const size_t total = recomp::allocation_size + 2 * recomp::rdram_guard_size;
    const size_t committed_end = recomp::rdram_guard_size + recomp::mem_size;
    const size_t views[] = {
        recomp::rdram_guard_size,
        recomp::rdram_guard_size + rdram_offset(kUncachedBase),
        recomp::rdram_guard_size + rdram_offset(kMainAliasBase),
    };
    struct Piece {
        size_t start, end;
        bool view;
    };
    std::vector<Piece> pieces;
    size_t cursor = 0;
    for (size_t view : views) {
        if (view > cursor) {
            pieces.push_back({cursor, view, false});
        }
        pieces.push_back({view, view + kRdramSize, true});
        cursor = view + kRdramSize;
    }
    pieces.push_back({cursor, total, false});

    // No other thread allocates memory here while the game is starting, so the range is still free
    // when it is reserved again a moment later.
    if (!VirtualFree(base, 0, MEM_RELEASE)) {
        print_windows_error("VirtualFree (rdram)");
        CloseHandle(section);
        return false;
    }
    if (virtual_alloc2(nullptr, base, total, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0) != base) {
        print_windows_error("VirtualAlloc2 (rdram placeholder)");
        CloseHandle(section);
        return false;
    }
    // Split the placeholder in front of every piece but the last.
    for (size_t i = 0; i + 1 < pieces.size(); i++) {
        if (!VirtualFree(base + pieces[i].start, pieces[i].end - pieces[i].start, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
            print_windows_error("VirtualFree (split rdram placeholder)");
            CloseHandle(section);
            return false;
        }
    }
    for (const Piece& piece : pieces) {
        uint8_t* const start = base + piece.start;
        const size_t size = piece.end - piece.start;
        if (piece.view) {
            if (map_view_of_file3(section, GetCurrentProcess(), start, 0, size, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0) != start) {
                print_windows_error("MapViewOfFile3 (rdram alias)");
                CloseHandle(section);
                return false;
            }
            continue;
        }
        if (virtual_alloc2(nullptr, start, size, MEM_RESERVE | MEM_REPLACE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0) != start) {
            print_windows_error("VirtualAlloc2 (rdram reservation)");
            CloseHandle(section);
            return false;
        }
        if (piece.start < committed_end) {
            const size_t end = piece.end < committed_end ? piece.end : committed_end;
            if (VirtualAlloc(start, end - piece.start, MEM_COMMIT, PAGE_READWRITE) == nullptr) {
                print_windows_error("VirtualAlloc (rdram commit)");
                CloseHandle(section);
                return false;
            }
        }
    }
    CloseHandle(section);  // the views keep it alive
    return true;
}
#endif

} // namespace

static const uint8_t* g_rdram = nullptr;

const uint8_t* current_rdram() { return g_rdram; }

bool install_rdram_alias(uint8_t* rdram) {
    g_rdram = rdram;
#if CONKER_PS5
    // One allocation at all three addresses. It is filled through the alias before it replaces the
    // memory at rdram itself, so what the runtime already placed there (the initial 1 MiB ROM copy and
    // the IPL3 variables) is kept, and direct memory, which is not cleared when it is allocated, holds
    // nothing else.
    int64_t direct = 0;
    if (!allocate_direct(kRdramSize, &direct, "sceKernelAllocateDirectMemory (rdram alias)")) {
        return false;
    }
    uint8_t* const alias = rdram + rdram_offset(kMainAliasBase);
    uint8_t* const uncached = rdram + rdram_offset(kUncachedBase);
    if (!map_direct_at(alias, kRdramSize, direct, "sceKernelMapDirectMemory (rdram alias)") ||
        !map_direct_at(uncached, kRdramSize, direct, "sceKernelMapDirectMemory (uncached rdram)")) {
        return false;
    }
    std::memcpy(alias, rdram, kRdramSize);
    if (!map_direct_at(rdram, kRdramSize, direct, "sceKernelMapDirectMemory (rdram)")) {
        return false;
    }
    g_backed = {
        {0, recomp::mem_size},
        {rdram_offset(kMainAliasBase), rdram_offset(kMainAliasBase) + kRdramSize},
        {rdram_offset(kUncachedBase), rdram_offset(kUncachedBase) + kRdramSize},
    };
    return true;
#elif defined(CONKER_PLATFORM_POSIX)
    // Preserve whatever the runtime already placed in RDRAM (the initial 1 MiB ROM copy and
    // the IPL3 variables) before swapping the backing memory out from under it.
    std::vector<uint8_t> saved(rdram, rdram + kRdramSize);

    char name[64];
    std::snprintf(name, sizeof(name), "/conker-recomp-rdram-%d", static_cast<int>(getpid()));
    shm_unlink(name);
    int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        std::perror("shm_open");
        return false;
    }
    shm_unlink(name);  // the mappings keep it alive
    if (ftruncate(fd, static_cast<off_t>(kRdramSize)) != 0) {
        std::perror("ftruncate");
        close(fd);
        return false;
    }

    const int prot = PROT_READ | PROT_WRITE;
    const int flags = MAP_SHARED | MAP_FIXED;
    void* low = mmap(rdram, kRdramSize, prot, flags, fd, 0);
    void* alias = mmap(rdram + rdram_offset(kMainAliasBase), kRdramSize, prot, flags, fd, 0);
    void* uncached = mmap(rdram + rdram_offset(kUncachedBase), kRdramSize, prot, flags, fd, 0);
    close(fd);
    if (low == MAP_FAILED || alias == MAP_FAILED || uncached == MAP_FAILED) {
        std::perror("mmap (rdram alias)");
        return false;
    }
    std::memcpy(rdram, saved.data(), saved.size());
    return true;
#elif defined(_WIN32)
    // Everything the runtime has written so far: the initial 1 MiB ROM copy, the IPL3 variables,
    // the PI handles at 0x80800000 and patch data from 0x80801000. Mods and the heap come later.
    constexpr size_t kWritten = 0x1000000;
    std::vector<uint8_t> saved(rdram, rdram + kWritten);
    if (!replace_rdram_reservation(rdram)) {
        return false;
    }
    std::memcpy(rdram, saved.data(), saved.size());
    return true;
#else
    std::fprintf(stderr, "[conker] RDRAM aliasing is not implemented on this platform yet\n");
    return false;
#endif
}

bool make_window_accessible(uint8_t* rdram, uint32_t vram, size_t size) {
#if CONKER_PS5
    // Direct memory of its own for each run of units that has none yet, cleared, as the other
    // platforms' newly accessible pages are.
    const size_t page = page_size();
    const size_t start = rdram_offset(vram) & ~(page - 1);
    const size_t end = (rdram_offset(vram) + size + page - 1) & ~(page - 1);
    size_t at = start;
    while (at < end) {
        if (is_backed(at)) {
            at += page;
            continue;
        }
        size_t run_end = at + page;
        while (run_end < end && !is_backed(run_end)) {
            run_end += page;
        }
        int64_t direct = 0;
        if (!allocate_direct(run_end - at, &direct, "sceKernelAllocateDirectMemory (rdram window)") ||
            !map_direct_at(rdram + at, run_end - at, direct, "sceKernelMapDirectMemory (rdram window)")) {
            return false;
        }
        std::memset(rdram + at, 0, run_end - at);
        g_backed.emplace_back(at, run_end);
        at = run_end;
    }
    return true;
#elif defined(CONKER_PLATFORM_POSIX)
    const size_t page = page_size();
    size_t start = rdram_offset(vram) & ~(page - 1);
    size_t end = (rdram_offset(vram) + size + page - 1) & ~(page - 1);
    if (mprotect(rdram + start, end - start, PROT_READ | PROT_WRITE) != 0) {
        std::perror("mprotect (rdram window)");
        return false;
    }
    return true;
#elif defined(_WIN32)
    const size_t page = page_size();
    size_t start = rdram_offset(vram) & ~(page - 1);
    size_t end = (rdram_offset(vram) + size + page - 1) & ~(page - 1);
    if (VirtualAlloc(rdram + start, end - start, MEM_COMMIT, PAGE_READWRITE) == nullptr) {
        print_windows_error("VirtualAlloc (rdram window)");
        return false;
    }
    return true;
#else
    (void)rdram; (void)vram; (void)size;
    std::fprintf(stderr, "[conker] rdram windows are not implemented on this platform yet\n");
    return false;
#endif
}

uint8_t* reserve_draw_image() {
    // Before the image, what RDRAM's reservation has (an address a little under 0); after its 4 GiB,
    // room for the largest thing drawn from the last offset (a frame buffer, 16 MiB at most).
    constexpr size_t kBefore = 64 * 1024;
    constexpr size_t kOffsets = size_t{1} << 32;
    constexpr size_t kAfter = 64 * 1024 * 1024;
    constexpr size_t kTotal = kBefore + kOffsets + kAfter;
#if CONKER_PS5
    // A kernel reservation with direct memory behind its first kDrawImageSize bytes, as rdram's
    // (tools/patches/n64modernruntime-ps5.patch). The address is a hint, and a range the kernel puts
    // elsewhere could be in the Vulkan driver's way (its GPU ring at 0x200000000-0x2FFFFFFFF, its
    // device memory from 0x4000000000), so only these are taken, 8 GiB apart: rdram has one of them,
    // and each image drawn from takes another.
    static_assert(kTotal <= 0x200000000ull);
    void* base = nullptr;
    for (uintptr_t candidate = 0x300000000ull; candidate + kTotal <= 0x4000000000ull && base == nullptr;
         candidate += 0x200000000ull) {
        void* placed = reinterpret_cast<void*>(candidate);
        if (sceKernelReserveVirtualRange(&placed, kTotal, 0, PS5_KERNEL_DIRECT_ALIGNMENT) != 0) {
            continue;
        }
        if (placed == reinterpret_cast<void*>(candidate)) {
            base = placed;
        } else {
            sceKernelMunmap(placed, kTotal);
        }
    }
    if (base == nullptr) {
        std::fprintf(stderr, "[conker] no address range for a draw image could be reserved\n");
        return nullptr;
    }
    // kBefore is one 64 KiB unit, so the image starts on one as direct memory must.
    static_assert(kBefore % PS5_KERNEL_DIRECT_ALIGNMENT == 0 && kDrawImageSize % PS5_KERNEL_DIRECT_ALIGNMENT == 0);
    uint8_t* const image = static_cast<uint8_t*>(base) + kBefore;
    int64_t direct = 0;
    if (!allocate_direct(kDrawImageSize, &direct, "sceKernelAllocateDirectMemory (draw image)")) {
        sceKernelMunmap(base, kTotal);
        return nullptr;
    }
    if (!map_direct_at(image, kDrawImageSize, direct, "sceKernelMapDirectMemory (draw image)")) {
        sceKernelMunmap(base, kTotal);
        sceKernelReleaseDirectMemory(direct, kDrawImageSize);
        return nullptr;
    }
    // Direct memory is not cleared when it is allocated.
    std::memset(image, 0, kDrawImageSize);
    return image;
#elif defined(CONKER_PLATFORM_POSIX)
    int flags = MAP_PRIVATE | MAP_ANON;
#if defined(MAP_NORESERVE)
    flags |= MAP_NORESERVE;
#endif
    void* const base = mmap(nullptr, kTotal, PROT_NONE, flags, -1, 0);
    if (base == MAP_FAILED) {
        std::perror("mmap (draw image)");
        return nullptr;
    }
    uint8_t* const image = static_cast<uint8_t*>(base) + kBefore;
    if (mprotect(image, kDrawImageSize, PROT_READ | PROT_WRITE) != 0) {
        std::perror("mprotect (draw image)");
        munmap(base, kTotal);
        return nullptr;
    }
    return image;
#elif defined(_WIN32)
    void* const base = VirtualAlloc(nullptr, kTotal, MEM_RESERVE, PAGE_NOACCESS);
    if (base == nullptr) {
        print_windows_error("VirtualAlloc (draw image)");
        return nullptr;
    }
    uint8_t* const image = static_cast<uint8_t*>(base) + kBefore;
    if (VirtualAlloc(image, kDrawImageSize, MEM_COMMIT, PAGE_READWRITE) == nullptr) {
        print_windows_error("VirtualAlloc (draw image commit)");
        VirtualFree(base, 0, MEM_RELEASE);
        return nullptr;
    }
    return image;
#else
    std::fprintf(stderr, "[conker] no draw image on this platform yet\n");
    return nullptr;
#endif
}

void write_image(uint8_t* rdram, uint32_t vram, std::span<const uint8_t> image) {
    // MEM_B(i, addr) is rdram[((addr + i) ^ 3) - 0xFFFFFFFF80000000]; the XOR commutes with the
    // base subtraction because the base has zero low bits.
    for (size_t i = 0; i < image.size(); i++) {
        rdram[rdram_offset(vram + static_cast<uint32_t>(i)) ^ 3] = image[i];
    }
}

} // namespace conker::memory
