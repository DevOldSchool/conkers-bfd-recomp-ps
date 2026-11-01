// Mod code (and game code a mod can steer) may leave a 64-bit value in a register and then load or
// store through it. Only the address's low 32 bits may count, as on the N64, so the access lands in
// the game's memory and never elsewhere in the process. These tests run each kind of access through
// the static recompiler's macros, LiveRecomp's generated code and ultramodern's TO_PTR, with an
// address register that is sign-extended (what the game makes), zero-extended, and with junk in the
// high word (what an attack would use).

#include <cstdio>
#include <cstring>
#include <string>
#include <sstream>
#include <vector>
#include <sys/mman.h>

#include "recompiler/live_recompiler.h"
#include "recomp.h"

#include "mod_sandbox_test.h"

namespace {
    // Where the accesses should land, as an offset into rdram.
    constexpr uint64_t target = 0x100000;

    // Three ways a register can hold that address. Without the fix a zero-extended address lands
    // 4 GiB past rdram, still inside the test's 8 GiB mapping, so the test reports the failure instead
    // of crashing. Junk in the high word would crash without the fix, so that case only runs once the
    // others pass.
    struct AddressCase {
        const char* name;
        gpr reg;
        bool may_crash;
    };
    constexpr AddressCase address_cases[] = {
        { "sign-extended", 0xFFFFFFFF80000000ULL + target, false },
        { "zero-extended", 0x0000000080000000ULL + target, false },
        { "junk high word", 0x1234567880000000ULL + target, true },
    };

    constexpr gpr store_value = 0x1122334455667788ULL;

    uint8_t* map_rdram() {
        // 8 GiB, so an address that escapes the low 32 bits by one bit still lands in the mapping.
        void* p = mmap(nullptr, 8ULL << 30, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
        return p == MAP_FAILED ? nullptr : static_cast<uint8_t*>(p);
    }

    // rdram holds words in host order, so a byte at game address a is at a ^ 3 and a halfword at a ^ 2.
    uint32_t word_at(uint8_t* rdram, uint64_t off) { uint32_t v; memcpy(&v, rdram + off, 4); return v; }
    uint16_t half_at(uint8_t* rdram, uint64_t off) { uint16_t v; memcpy(&v, rdram + (off ^ 2), 2); return v; }
    uint8_t byte_at(uint8_t* rdram, uint64_t off) { return rdram[off ^ 3]; }
    void set_word(uint8_t* rdram, uint64_t off, uint32_t v) { memcpy(rdram + off, &v, 4); }
    void set_half(uint8_t* rdram, uint64_t off, uint16_t v) { memcpy(rdram + (off ^ 2), &v, 2); }
    void set_byte(uint8_t* rdram, uint64_t off, uint8_t v) { rdram[off ^ 3] = v; }

    // The values loads read, at target + 0x20 and up, apart from where the stores go so a load can
    // not pass by reading back a store that went to the wrong place.
    void fill_load_data(uint8_t* rdram) {
        memset(rdram + target, 0, 0x40);
        set_word(rdram, target + 0x20, 0xCAFEF00D);
        set_half(rdram, target + 0x24, 0xBEEF);
        set_word(rdram, target + 0x28, 0x01020304);
        set_word(rdram, target + 0x2C, 0x05060708);
        set_byte(rdram, target + 0x30, 0x5A);
    }

    struct Results {
        gpr lw, lwl, ld, lhu, lbu;
    };

    // Checks what the stores wrote and what the loads read.
    void check(Checker& c, const std::string& what, uint8_t* rdram, const Results& r) {
        c.expect(word_at(rdram, target + 0x00) == 0x55667788, what + ": sw lands at the address");
        c.expect(half_at(rdram, target + 0x04) == 0x7788, what + ": sh lands at the address");
        c.expect(byte_at(rdram, target + 0x06) == 0x88, what + ": sb lands at the address");
        c.expect(word_at(rdram, target + 0x08) == 0x11223344 && word_at(rdram, target + 0x0C) == 0x55667788,
            what + ": sd lands at the address");
        c.expect(word_at(rdram, target + 0x10) == 0x55667788, what + ": swl lands at the address");
        c.expect(r.lw == 0xFFFFFFFFCAFEF00DULL, what + ": lw reads from the address");
        c.expect(r.lwl == 0xFFFFFFFFCAFEF00DULL, what + ": lwl reads from the address");
        c.expect(r.ld == 0x0102030405060708ULL, what + ": ld reads from the address");
        c.expect(r.lhu == 0xBEEF, what + ": lhu reads from the address");
        c.expect(r.lbu == 0x5A, what + ": lbu reads from the address");
    }

    // The static recompiler's output uses these macros for every access.
    void run_macros(uint8_t* rdram, gpr reg, Results& r) {
        MEM_W(0x00, reg) = (int32_t)store_value;
        MEM_H(0x04, reg) = (int16_t)store_value;
        MEM_B(0x06, reg) = (int8_t)store_value;
        SD(store_value, 0x08, reg);
        do_swl(rdram, 0x10, reg, store_value);
        r.lw = (gpr)(int32_t)MEM_W(0x20, reg);
        r.lwl = do_lwl(rdram, 0, 0x20, reg);
        r.ld = LD(0x28, reg);
        r.lhu = MEM_HU(0x24, reg);
        r.lbu = MEM_BU(0x30, reg);
    }

    // The same accesses as MIPS code for LiveRecomp, with the address in $a0 and the value in $a1.
    constexpr uint32_t live_code[] = {
        0xAC850000, // sw   $a1, 0x00($a0)
        0xA4850004, // sh   $a1, 0x04($a0)
        0xA0850006, // sb   $a1, 0x06($a0)
        0xFC850008, // sd   $a1, 0x08($a0)
        0xA8850010, // swl  $a1, 0x10($a0)
        0x8C820020, // lw   $v0, 0x20($a0)
        0x88830020, // lwl  $v1, 0x20($a0)
        0xDC860028, // ld   $a2, 0x28($a0)
        0x94870024, // lhu  $a3, 0x24($a0)
        0x90880030, // lbu  $t0, 0x30($a0)
        0x03E00008, // jr   $ra
        0x00000000, // nop
    };
    constexpr uint32_t live_code_vram = 0x80001000;

    void switch_error(const char*, uint32_t, uint32_t) {}
    recomp_func_t* get_function(int32_t) { return nullptr; }

    recomp_func_t* compile_live(N64Recomp::LiveGeneratorOutput& output) {
        N64Recomp::Context context{};
        std::vector<uint32_t> words;
        for (uint32_t w : live_code) {
            words.push_back(__builtin_bswap32(w)); // as the words sit in a big-endian ROM
        }
        context.rom.resize(sizeof(live_code));
        memcpy(context.rom.data(), words.data(), sizeof(live_code));

        context.sections.resize(1);
        context.sections[0].ram_addr = live_code_vram;
        context.sections[0].rom_addr = 0;
        context.sections[0].size = sizeof(live_code);
        context.sections[0].name = ".text";
        context.sections[0].executable = true;
        context.sections[0].relocatable = true;
        context.sections[0].function_addrs.push_back(live_code_vram);
        context.section_functions.resize(1);
        context.section_functions[0].push_back(0);
        context.functions_by_vram[live_code_vram].push_back(0);
        context.functions.emplace_back(live_code_vram, 0, words, "address_window_test", 0);

        int32_t section_addresses[] = { (int32_t)live_code_vram };
        N64Recomp::LiveGeneratorInputs inputs{};
        inputs.switch_error = switch_error;
        inputs.get_function = get_function;
        inputs.local_section_addresses = section_addresses;

        N64Recomp::LiveGenerator generator{ context.functions.size(), inputs };
        std::ostringstream unused_output{};
        std::vector<std::vector<uint32_t>> unused_static_funcs{};
        if (!N64Recomp::recompile_function_live(generator, context, 0, unused_output, unused_static_funcs, true)) {
            return nullptr;
        }
        output = generator.finish();
        return output.good && !output.functions.empty() ? output.functions[0] : nullptr;
    }

    void run_live(recomp_func_t* func, uint8_t* rdram, gpr reg, Results& r) {
        recomp_context ctx{};
        ctx.r4 = reg;
        ctx.r5 = store_value;
        ctx.r29 = 0xFFFFFFFF80000000ULL + 0x200000; // a stack, unused by the code
        func(rdram, &ctx);
        r.lw = ctx.r2;
        r.lwl = ctx.r3;
        r.ld = ctx.r6;
        r.lhu = ctx.r7;
        r.lbu = ctx.r8;
    }
}

void test_address_window(Checker& c) {
    uint8_t* rdram = map_rdram();
    if (!c.expect(rdram != nullptr, "map 8 GiB for rdram")) {
        return;
    }

    N64Recomp::live_recompiler_init();
    N64Recomp::LiveGeneratorOutput output{};
    recomp_func_t* live_func = compile_live(output);
    c.expect(live_func != nullptr, "LiveRecomp compiles the test code");

    for (const AddressCase& ac : address_cases) {
        if (ac.may_crash && c.failures() != 0) {
            printf("skipped  %s (earlier failures)\n", ac.name);
            continue;
        }

        Results r{};
        fill_load_data(rdram);
        run_macros(rdram, ac.reg, r);
        check(c, std::string("macros, ") + ac.name, rdram, r);

        if (live_func != nullptr) {
            r = {};
            fill_load_data(rdram);
            run_live(live_func, rdram, ac.reg, r);
            check(c, std::string("LiveRecomp, ") + ac.name, rdram, r);
        }

        fill_load_data(rdram);
        set_word(rdram, target, 0x600DF00D);
        c.expect(*to_ptr_u32(rdram, ac.reg) == 0x600DF00D, std::string("TO_PTR, ") + ac.name);
    }

    munmap(rdram, 8ULL << 30);
}
