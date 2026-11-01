// Registers the recompiled sections with librecomp's function lookup.
//
// librecomp's own init loads every section whose ROM range falls inside the first 1 MiB at the
// IPL3 load address, so main's functions become reachable at 0x8000xxxx automatically. Conker
// also reaches them through the 0x10000000 TLB alias, and the game and debugger overlays are
// not in that first megabyte at all, so the remaining addresses are added here.
#include <cstddef>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "recomp.h"
#include "funcs.h"
#include "librecomp/mods.hpp"
#include "librecomp/overlays.hpp"

#include "recomp_overlays.inl"

#include "mod_locks.h"

#include "runtime/game.hpp"
#include "runtime/memory.hpp"
#include "runtime/trace.h"

// librecomp's record of mod replacements (tools/patches/n64modernruntime-mods.patch).
extern "C" recomp_func_t* recomp_get_function_redirect(recomp_func_t* func);

// A recompiled function that finds the generation changed asks here, by its own name, what a mod
// put in its place (src/runtime/trace.h).
extern "C" recomp_func_t* conker_find_redirect(const char* name) {
    static const std::unordered_map<std::string_view, recomp_func_t*> functions_by_name{
#include "func_names.inl"
    };
    auto find_it = functions_by_name.find(name);
    return find_it == functions_by_name.end() ? nullptr : recomp_get_function_redirect(find_it->second);
}

namespace conker {

// librecomp refuses to hook a function whose ROM size is 0 (it has no MIPS to recompile again), and
// our librecomp patch refuses to let a mod replace one too: so the functions config/us.toml
// changes, whose changes a hook or replacement would drop, are given size 0 here.
void lock_changed_functions() {
    for (uint32_t vram : kModLockedFunctions) {
        for (size_t section_index = 0; section_index < ARRLEN(section_table); section_index++) {
            SectionTableEntry& section = section_table[section_index];
            if (vram < section.ram_addr || vram >= section.ram_addr + section.size) {
                continue;
            }
            for (size_t i = 0; i < section.num_funcs; i++) {
                if (section.ram_addr + section.funcs[i].offset == vram) {
                    section.funcs[i].rom_size = 0;
                }
            }
        }
    }
}

void register_overlays() {
    lock_changed_functions();
    // A hooked function is recompiled again; its switch's jump table is usually in the data.
    std::vector<recomp::mods::JumpTableData> jump_table_data;
    for (const auto& data : kModJumpTableData) {
        jump_table_data.push_back({data.ram_addr, data.size, data.rom_addr});
    }
    recomp::mods::set_jump_table_data(std::move(jump_table_data));
    // A mod runs its own code in the game, so a newly installed one waits for the player to turn it
    // on (the debug menu's MODS page, which says to trust where it came from).
    recomp::mods::set_enable_new_mods(false);
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
