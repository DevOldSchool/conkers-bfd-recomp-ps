#include "runtime/game.hpp"

#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "funcs.h"
#include "ultramodern/ultramodern.hpp"
#include "runtime/memory.hpp"
#include "runtime/rzip.hpp"

namespace conker {

recomp::GameEntry make_game_entry() {
    recomp::GameEntry entry{};
    entry.rom_hash = kUsRomHash;
    entry.internal_name = "CONKER BFD";
    entry.display_name = "Conker's Bad Fur Day (US)";
    entry.game_id = kGameId;
    entry.mod_game_id = "conker";
    entry.save_type = recomp::SaveType::Eep16k;  // NFUE uses a 16 kbit EEPROM
    entry.is_enabled = true;
    entry.has_compressed_code = true;  // the game overlay is RZIP-compressed in ROM
    entry.entrypoint_address = static_cast<gpr>(static_cast<int32_t>(0x80001000));
    entry.entrypoint = entrypoint;
    entry.on_init_callback = on_init;
    entry.thread_create_callback = [](uint8_t*, recomp_context* ctx) {
        set_current_context(ctx);
        use_fr1_float_mode(ctx);
    };
    return entry;
}

// IPL3 leaves CP0 Status at 0x34000000 (CU0 | CU1 | FR) and libultra keeps FR set, so the game
// runs with 32 independent single-precision FPU registers. Rare's handwritten geometry code
// relies on that: it does arithmetic on odd registers ($f21, $f23, ...) and saves $f20..$f31 as
// twelve separate singles. librecomp starts every thread in the FR=0 layout, where an odd
// register is the upper half of the even one, so the generated `ctx->f_odd[...]` loads and the
// `ctx->f23.fl` arithmetic would address different storage. Switch each N64 thread to FR=1
// before it runs any recompiled code.
void use_fr1_float_mode(recomp_context* ctx) {
    constexpr uint32_t kStatusFr = 0x04000000;
    ctx->status_reg |= kStatusFr;
    ctx->f_odd = &ctx->f1.u32l;
    ctx->mips3_float_mode = 1;
}

static thread_local recomp_context* t_current_context = nullptr;

void set_current_context(recomp_context* ctx) { t_current_context = ctx; }
recomp_context* current_context() { return t_current_context; }

void entrypoint(uint8_t* rdram, recomp_context* ctx) {
    set_current_context(ctx);
    use_fr1_float_mode(ctx);
    // ROM 0x1000: clear bss and set the stack pointer.
    std::memset(rdram + memory::rdram_offset(memory::kBssStart), 0, memory::kBssSize);
    ctx->r29 = static_cast<gpr>(static_cast<int32_t>(memory::kInitialSp));

    // ROM 0x5AB0 installs the 0x10000000 TLB alias (done natively in on_init) and jumps to
    // 0x10001050, which is func_80001050 in the decomp.
    func_80001050(rdram, ctx);
}

void on_init(uint8_t* rdram, recomp_context* /*ctx*/) {
    if (!memory::install_rdram_alias(rdram)) {
        throw std::runtime_error("failed to install the 0x10000000 RDRAM alias");
    }

    std::span<const uint8_t> rom = recomp::get_rom();
    if (rom.size() < memory::kGameArchiveEnd) {
        throw std::runtime_error("ROM is too small to contain the game archive");
    }

    // The game overlay is demand-paged through the TLB on hardware. Here the whole image is
    // made resident up front; the paging routines in main are stubbed out.
    rzip::GameArchive archive = rzip::parse_game_archive(
        rom.subspan(memory::kGameArchiveRom, memory::kGameArchiveEnd - memory::kGameArchiveRom));
    std::printf("[conker] game archive: %zu code chunks, %zu code bytes, %zu data bytes\n",
                archive.code_chunk_count, archive.code.size(), archive.data.size());

    if (!memory::make_window_accessible(rdram, memory::kGameCodeVram, archive.code.size())) {
        throw std::runtime_error("failed to map the game code window");
    }
    memory::write_image(rdram, memory::kGameCodeVram, archive.code);

    // Main's startup inflates the game data itself (through recompiled code), so it is not
    // written here; doing so would be harmless but would hide a failure in that path.

    if (!memory::make_window_accessible(rdram, memory::kDebuggerVram, memory::kDebuggerSize)) {
        throw std::runtime_error("failed to map the debugger window");
    }
    memory::write_image(rdram, memory::kDebuggerVram, rom.subspan(memory::kDebuggerRom, memory::kDebuggerSize));

    // Uncached (KSEG1) windows outside the runtime's committed 512 MiB: RCP registers at
    // 0xA4000000 (reads return 0, writes are absorbed; register-driven DMA is replaced by
    // native routines, see ultra_extras.cpp), the cartridge ROM at
    // 0xB0000000, cart domain-2 registers at 0xBC000000 (func_800038E0 points there) and PIF RAM
    // at 0xBFC00000. The ROM copy lets direct ROM reads work; the others absorb hardware pokes.
    constexpr uint32_t kRomKseg1 = 0xB0000000, kCartDom2 = 0xBC000000, kPifRam = 0xBFC00000;
    constexpr uint32_t kRcpRegisters = 0xA4000000;  // SP, DP, MI, VI, AI, PI, RI, SI registers
    if (!memory::make_window_accessible(rdram, kRcpRegisters, 0x900000) ||
        !memory::make_window_accessible(rdram, kRomKseg1, rom.size()) ||
        !memory::make_window_accessible(rdram, kCartDom2, 0x1000) ||
        !memory::make_window_accessible(rdram, kPifRam, 0x1000)) {
        throw std::runtime_error("failed to map the KSEG1 windows");
    }
    memory::write_image(rdram, kRomKseg1, rom);

    // IPL3 variables librecomp's generic init leaves at zero. The idle thread only starts the
    // main thread when osCicId reports the CIC-6105 Conker shipped with (func_800010F8).
    const gpr os_cic_id = static_cast<gpr>(static_cast<int32_t>(0x80000310));  // MEM_* want sign-extended addresses
    MEM_W(0, os_cic_id) = 0x17D9;

    // Rare's code reads the current thread through libultra's `__osRunningThread` (0x8002BE00,
    // following __osActiveQueue at 0x8002BDFC) instead of the OS accessors.
    ultramodern::set_running_thread_address(static_cast<int32_t>(0x8002BE00));

    register_runtime_addresses();
}

} // namespace conker
