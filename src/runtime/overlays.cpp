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

void register_runtime_addresses() {
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
