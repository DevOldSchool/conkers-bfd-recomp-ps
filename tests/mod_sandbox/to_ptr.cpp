#include "ultramodern/ultra64.h"

#include "mod_sandbox_test.h"

uint32_t* to_ptr_u32(uint8_t* rdram, uint64_t reg) {
    return TO_PTR(uint32_t, reg);
}
