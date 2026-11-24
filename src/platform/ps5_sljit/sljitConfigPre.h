// sljit's configuration in the PS5 title (SLJIT_HAVE_CONFIG_PRE, CMakeLists.txt): LiveRecomp's code for
// mods goes in executable memory from the SDK platform layer (ps5_sljit.c), not sljit's own allocator.
// A probe title on the console ran both, but sljit's anonymous mmap lands in the range the
// Vulkan driver maps GPU memory in; the platform layer's direct memory is placed outside it.
#define SLJIT_EXECUTABLE_ALLOCATOR 0
void* conker_ps5_sljit_malloc_exec(unsigned long size);
void conker_ps5_sljit_free_exec(void* pointer);
#define SLJIT_MALLOC_EXEC(size, exec_allocator_data) conker_ps5_sljit_malloc_exec(size)
#define SLJIT_FREE_EXEC(pointer, exec_allocator_data) conker_ps5_sljit_free_exec(pointer)
