// Executable memory for LiveRecomp's sljit in the PS5 title (sljitConfigPre.h beside this file). The
// SDK platform layer's direct memory, mapped read-write and given execute with sceKernelMprotect: the
// route the console grants a title (its JIT interface returns 0x80020001), checked on the console (firmware
// 13.59) with a probe title. LiveRecomp generates one block per mod, so a 64 KiB unit per block is no waste.
#include <stdint.h>
#include <stdio.h>

#include <ps5platform/exec.h>

void* conker_ps5_sljit_malloc_exec(unsigned long size) {
    // Near this code, outside the GPU window.
    void* code = ps5_exec_allocate(size, (uintptr_t)&conker_ps5_sljit_malloc_exec);
    if (code == NULL) {
        fprintf(stderr, "[conker] ps5: no executable memory for %lu bytes of mod code\n", size);
    }
    return code;
}

void conker_ps5_sljit_free_exec(void* pointer) {
    ps5_exec_release(pointer);
}
