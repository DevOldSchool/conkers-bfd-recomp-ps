#include "runtime/memory.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

#if defined(CONKER_PLATFORM_POSIX)
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
#if defined(CONKER_PLATFORM_POSIX)
    return static_cast<size_t>(sysconf(_SC_PAGESIZE));
#else
    return 4096;
#endif
}

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
#if defined(CONKER_PLATFORM_POSIX)
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
#if defined(CONKER_PLATFORM_POSIX)
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

void write_image(uint8_t* rdram, uint32_t vram, std::span<const uint8_t> image) {
    // MEM_B(i, addr) is rdram[((addr + i) ^ 3) - 0xFFFFFFFF80000000]; the XOR commutes with the
    // base subtraction because the base has zero low bits.
    for (size_t i = 0; i < image.size(); i++) {
        rdram[rdram_offset(vram + static_cast<uint32_t>(i)) ^ 3] = image[i];
    }
}

} // namespace conker::memory
