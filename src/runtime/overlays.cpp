// Registers the recompiled sections with librecomp's function lookup.
//
// librecomp's own init loads every section whose ROM range falls inside the first 1 MiB at the
// IPL3 load address, so main's functions become reachable at 0x8000xxxx automatically. Conker
// also reaches them through the 0x10000000 TLB alias, and the game and debugger overlays are
// not in that first megabyte at all, so the remaining addresses are added here.
#include <cstddef>

#include "recomp.h"
#include "funcs.h"
#include "librecomp/overlays.hpp"

#include "recomp_overlays.inl"

#include "runtime/game.hpp"
#include "runtime/memory.hpp"

namespace conker {

void register_overlays() {
    recomp::overlays::overlay_section_table_data_t sections{
        .code_sections = section_table,
        .num_code_sections = ARRLEN(section_table),
        .total_num_sections = num_sections,
    };
    recomp::overlays::overlays_by_index_t overlays{
        .table = overlay_sections_by_index,
        .len = ARRLEN(overlay_sections_by_index),
    };
    recomp::overlays::register_overlays(sections, overlays);
}

// Main's libc routines that config/us.toml leaves out of the recompiled code, so they are not in the
// section table: src/runtime/ultra_extras.cpp has them natively. Main's addresses (as 0x1000xxxx).
extern "C" {
void memcpy_recomp(uint8_t* rdram, recomp_context* ctx);
void strlen_recomp(uint8_t* rdram, recomp_context* ctx);
void strchr_recomp(uint8_t* rdram, recomp_context* ctx);
void lldiv_recomp(uint8_t* rdram, recomp_context* ctx);
void ldiv_recomp(uint8_t* rdram, recomp_context* ctx);
void bcopy_recomp(uint8_t* rdram, recomp_context* ctx);
void sqrtf_recomp(uint8_t* rdram, recomp_context* ctx);
}

namespace {

constexpr struct {
    uint32_t address;
    recomp_func_t* func;
} kNativeLibc[] = {
    {0x10022EC0, memcpy_recomp}, {0x10022EEC, strlen_recomp}, {0x10022F14, strchr_recomp}, {0x10022F60, lldiv_recomp},
    {0x10023060, ldiv_recomp},   {0x10023A10, bcopy_recomp},  {0x10026530, sqrtf_recomp},
};

} // namespace

void register_runtime_addresses() {
    for (const auto& native : kNativeLibc) {
        // Reachable at 0x8000xxxx too, as the rest of main is through librecomp's own init.
        recomp::overlays::add_loaded_function(static_cast<int32_t>(native.address), native.func);
        recomp::overlays::add_loaded_function(static_cast<int32_t>(native.address - memory::kMainAliasBase + 0x80000000u), native.func);
    }
    for (size_t section_index = 0; section_index < ARRLEN(section_table); section_index++) {
        const SectionTableEntry& section = section_table[section_index];
        // Every section is described at its runtime address in the symbol file, so registering at
        // `ram_addr` covers 0x1000xxxx (main alias), 0x15xxxxxx (game) and 0x16xxxxxx (debugger).
        for (size_t i = 0; i < section.num_funcs; i++) {
            const FuncEntry& func = section.funcs[i];
            recomp::overlays::add_loaded_function(static_cast<int32_t>(section.ram_addr + func.offset), func.func);
        }
    }
}

} // namespace conker
