#include "memory.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

#if defined(CONKER_PLATFORM_POSIX)
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
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
