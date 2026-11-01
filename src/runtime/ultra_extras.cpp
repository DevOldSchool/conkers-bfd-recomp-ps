// Native implementations of libultra internals and libc helpers that the recompiled Conker code
// calls directly. N64Recomp renames these to `<name>_recomp` and leaves them to the runtime;
// librecomp covers the common ones, this file covers what Conker additionally needs.
//
// Conventions (o32 ABI): arguments in r4..r7 then the stack at 0x10(sp); 64-bit arguments in
// even/odd register pairs (high word first); return in r2 (r2:r3 for 64-bit); floats in f12/f0.
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>

#include "recomp.h"
#include "librecomp/game.hpp"
#include "ultramodern/ultramodern.hpp"

#include "runtime/game.hpp"
#include "platform/input.hpp"
#include "runtime/frame_input.hpp"
#include "runtime/memory.hpp"
#include "net/net_play.hpp"
#include "runtime/snapshot.hpp"

namespace {

#define ARG0 (ctx->r4)
#define ARG1 (ctx->r5)
#define ARG2 (ctx->r6)
#define ARG3 (ctx->r7)
#define RET(value) (ctx->r2 = static_cast<gpr>(static_cast<int32_t>(value)))

inline uint32_t as_u32(gpr value) { return static_cast<uint32_t>(value); }

void report_once(const char* name, const char* detail = nullptr) {
    static std::mutex mutex;
    static std::set<std::string> seen;
    std::lock_guard lock{mutex};
    if (seen.insert(name).second) {
        std::printf("[conker] %s: %s\n", name, detail ? detail : "native stub");
    }
}

// A handful of native structures live at the top of the 8 MiB RDRAM, above anything Conker
// (a 4 MiB game) touches.
constexpr uint32_t kViContextScratch = 0x807F0000;

// --- PIF shim -------------------------------------------------------------------------------
// Rare's EEPROM and controller-pak code bypasses libultra's high-level routines and talks to
// the PIF through __osSiRawStartDma. The 64-byte PIF RAM is modelled here with a tiny command
// interpreter: a controller on each of channels 0-3 that has one (conker::frame_input::connected), a
// 16 kbit EEPROM on channel 4, and "no device" for everything else. EEPROM contents are volatile for now.
std::mutex pif_mutex;
uint8_t pif_ram[64];
uint8_t eeprom_image[2048];
// Native state the game's memory depends on, carried by a snapshot (runtime/snapshot.hpp).
const bool g_snapshot_fields = (conker::snapshot::add("ultra_extras.eeprom_image", eeprom_image, sizeof(eeprom_image)), true);

void pif_process_commands() {
    size_t i = 0;
    int channel = 0;
    while (i < sizeof(pif_ram)) {
        const uint8_t head = pif_ram[i];
        if (head == 0xFE) {
            break;
        }
        if (head == 0x00) {  // skip channel
            channel++;
            i++;
            continue;
        }
        if (head == 0xFF) {  // padding
            i++;
            continue;
        }
        const size_t tx = head & 0x3F;
        if (i + 1 >= sizeof(pif_ram)) {
            break;
        }
        const size_t rx = pif_ram[i + 1] & 0x3F;
        uint8_t* cmd = &pif_ram[i + 2];
        uint8_t* resp = cmd + tx;
        if (i + 2 + tx + rx > sizeof(pif_ram)) {
            break;
        }
        bool handled = false;
        if (channel < 4 && conker::frame_input::connected(channel)) {
            if (cmd[0] == 0x00 || cmd[0] == 0xFF) {  // status / reset
                if (rx >= 3) {  // controller; a pak when the host pad can rumble
                    resp[0] = 0x05;
                    resp[1] = 0x00;
                    resp[2] = conker::frame_input::has_rumble(channel) ? 0x01 : 0x00;
                    handled = true;
                }
            } else if (cmd[0] == 0x01) {  // read buttons
                if (rx >= 4) {
                    report_once("PIF read buttons", "controllers read through raw SI DMA");
                    const conker::frame_input::Port port = conker::frame_input::port(channel);
                    resp[0] = static_cast<uint8_t>(port.buttons >> 8);
                    resp[1] = static_cast<uint8_t>(port.buttons);
                    resp[2] = static_cast<uint8_t>(port.stick_x);
                    resp[3] = static_cast<uint8_t>(port.stick_y);
                    handled = true;
                }
            }
            // 0x02/0x03 pak read/write: unanswered -> error below. The game reaches the rumble pak
            // through func_800057E0 and friends instead, which are native.
        } else if (channel == 4) {
            if (cmd[0] == 0x00 || cmd[0] == 0xFF) {
                if (rx >= 3) { resp[0] = 0x00; resp[1] = 0xC0; resp[2] = 0x00; handled = true; }  // 16 kbit EEPROM
            } else if (cmd[0] == 0x04 && tx >= 2) {  // read 8-byte block
                const size_t block = cmd[1] & 0xFF;
                if (rx >= 8 && block * 8 + 8 <= sizeof(eeprom_image)) {
                    report_once("PIF EEPROM read", "blank image");
                    std::memcpy(resp, &eeprom_image[block * 8], 8);
                    handled = true;
                }
            } else if (cmd[0] == 0x05 && tx >= 10) {  // write 8-byte block
                const size_t block = cmd[1] & 0xFF;
                if (block * 8 + 8 <= sizeof(eeprom_image)) {
                    report_once("PIF EEPROM write", "kept in memory only");
                    std::memcpy(&eeprom_image[block * 8], &cmd[2], 8);
                    if (rx >= 1) { resp[0] = 0x00; }
                    handled = true;
                }
            }
        }
        if (!handled) {
            pif_ram[i + 1] |= 0x80;  // no response from device
        }
        channel++;
        i += 2 + tx + rx;
    }
}

} // namespace

extern "C" {

// --- libc -------------------------------------------------------------------------------------

void memcpy_recomp(uint8_t* rdram, recomp_context* ctx) {
    gpr dst = ARG0, src = ARG1;
    const uint32_t n = as_u32(ARG2);
    for (uint32_t i = 0; i < n; i++) {
        MEM_B(i, dst) = MEM_B(i, src);
    }
    ctx->r2 = dst;
}

void bcopy_recomp(uint8_t* rdram, recomp_context* ctx) {
    gpr src = ARG0, dst = ARG1;
    const uint32_t n = as_u32(ARG2);
    if (as_u32(dst) > as_u32(src) && as_u32(dst) < as_u32(src) + n) {
        for (uint32_t i = n; i-- > 0;) {
            MEM_B(i, dst) = MEM_B(i, src);
        }
    } else {
        for (uint32_t i = 0; i < n; i++) {
            MEM_B(i, dst) = MEM_B(i, src);
        }
    }
}

void strlen_recomp(uint8_t* rdram, recomp_context* ctx) {
    gpr s = ARG0;
    uint32_t n = 0;
    while (MEM_B(n, s) != 0) {
        n++;
    }
    RET(n);
}

void strchr_recomp(uint8_t* rdram, recomp_context* ctx) {
    gpr s = ARG0;
    const int8_t c = static_cast<int8_t>(ARG1);
    for (uint32_t i = 0;; i++) {
        const int8_t cur = MEM_B(i, s);
        if (cur == c) {
            RET(as_u32(s) + i);
            return;
        }
        if (cur == 0) {
            RET(0);
            return;
        }
    }
}

void sqrtf_recomp(uint8_t* /*rdram*/, recomp_context* ctx) {
    ctx->f0.fl = std::sqrt(ctx->f12.fl);
}

void ldiv_recomp(uint8_t* rdram, recomp_context* ctx) {
    // ldiv_t* result (hidden), long num, long denom
    gpr result = ARG0;
    const int32_t num = static_cast<int32_t>(ARG1), denom = static_cast<int32_t>(ARG2);
    MEM_W(0, result) = denom ? num / denom : 0;
    MEM_W(4, result) = denom ? num % denom : 0;
    ctx->r2 = result;
}

void lldiv_recomp(uint8_t* rdram, recomp_context* ctx) {
    // lldiv_t* result (hidden), long long num (a2:a3), long long denom (0x10(sp):0x14(sp))
    gpr result = ARG0;
    const int64_t num = static_cast<int64_t>((static_cast<uint64_t>(as_u32(ARG2)) << 32) | as_u32(ARG3));
    const uint32_t denom_hi = static_cast<uint32_t>(MEM_W(0x10, ctx->r29));
    const uint32_t denom_lo = static_cast<uint32_t>(MEM_W(0x14, ctx->r29));
    const int64_t denom = static_cast<int64_t>((static_cast<uint64_t>(denom_hi) << 32) | denom_lo);
    const int64_t quot = denom ? num / denom : 0;
    const int64_t rem = denom ? num % denom : 0;
    MEM_W(0, result) = static_cast<int32_t>(quot >> 32);
    MEM_W(4, result) = static_cast<int32_t>(quot);
    MEM_W(8, result) = static_cast<int32_t>(rem >> 32);
    MEM_W(12, result) = static_cast<int32_t>(rem);
    ctx->r2 = result;
}

// --- CP0 status (Conker's entry function toggles FPU bits) ----------------------------------

void __osGetSR_recomp(uint8_t* /*rdram*/, recomp_context* ctx) {
    RET(0x24000000);  // CU1 | FR
}

void __osSetSR_recomp(uint8_t* /*rdram*/, recomp_context* /*ctx*/) {}

// --- VI: Rare runs its own VI manager; ultramodern tracks framebuffers via osViSwapBuffer -----

void __osViInit_recomp(uint8_t* /*rdram*/, recomp_context* /*ctx*/) { report_once("__osViInit"); }
void __osViSwapContext_recomp(uint8_t* /*rdram*/, recomp_context* /*ctx*/) {}
void __osViGetCurrentContext_recomp(uint8_t* /*rdram*/, recomp_context* ctx) { RET(kViContextScratch); }

// --- timers / threads: ultramodern owns scheduling --------------------------------------------

void __osTimerServicesInit_recomp(uint8_t* /*rdram*/, recomp_context* /*ctx*/) { report_once("__osTimerServicesInit"); }
void __osTimerInterrupt_recomp(uint8_t* /*rdram*/, recomp_context* /*ctx*/) { report_once("__osTimerInterrupt"); }
void __osSetTimerIntr_recomp(uint8_t* /*rdram*/, recomp_context* /*ctx*/) { report_once("__osSetTimerIntr", "ignored; timers are native"); }
void __osInsertTimer_recomp(uint8_t* /*rdram*/, recomp_context* ctx) { report_once("__osInsertTimer", "ignored; timers are native"); RET(0); }

void __osEnqueueThread_recomp(uint8_t* rdram, recomp_context* ctx) {
    ultramodern::thread_queue_insert(rdram, static_cast<int32_t>(ARG0), static_cast<int32_t>(ARG1));
}

void __osPopThread_recomp(uint8_t* rdram, recomp_context* ctx) {
    RET(ultramodern::thread_queue_pop(rdram, static_cast<int32_t>(ARG0)));
}

// --- SP: Rare submits RSP tasks with raw DMA to DMEM 0xFC0 followed by a status write ---------

static std::mutex sp_mutex;
static int32_t sp_pending_task = 0;

void __osSpDeviceBusy_recomp(uint8_t* /*rdram*/, recomp_context* ctx) { RET(0); }

void __osSpRawStartDma_recomp(uint8_t* /*rdram*/, recomp_context* ctx) {
    const uint32_t direction = as_u32(ARG0), dev_addr = as_u32(ARG1), dram_addr = as_u32(ARG2), size = as_u32(ARG3);
    if (direction == 1 /*OS_WRITE*/ && (dev_addr & 0x1FFF) == 0x0FC0 && size == sizeof(OSTask)) {
        std::lock_guard lock{sp_mutex};
        sp_pending_task = static_cast<int32_t>(dram_addr);
    }
    RET(0);
}

void __osSpSetStatus_recomp(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t status = as_u32(ARG0);
    constexpr uint32_t SP_CLR_HALT = 0x1;
    if (status & SP_CLR_HALT) {
        int32_t task = 0;
        {
            std::lock_guard lock{sp_mutex};
            task = sp_pending_task;
            sp_pending_task = 0;
        }
        if (task != 0) {
            ultramodern::submit_rsp_task(rdram, task);
        } else {
            report_once("__osSpSetStatus", "RSP started without a task DMA'd to DMEM 0xFC0");
        }
    }
}

// --- PI / AI ----------------------------------------------------------------------------------

void __osPiCreateAccessQueue_recomp(uint8_t* /*rdram*/, recomp_context* /*ctx*/) {}
void __osAiDeviceBusy_recomp(uint8_t* /*rdram*/, recomp_context* ctx) { RET(0); }

void osPiReadIo_recomp(uint8_t* rdram, recomp_context* ctx);

void osPiRawReadIo_recomp(uint8_t* rdram, recomp_context* ctx) {
    // libultra reads `osRomBase | devAddr`, so a ROM offset, a cartridge physical address and a
    // KSEG1 address all reach the same word. Conker's scene loader (func_15002FB4) checks the
    // word at 0xB0000A5C and halves the scene's trigger count when it does not match, which
    // drops the trigger that starts the intro cutscene.
    const uint32_t dev_addr = (0xB0000000u | as_u32(ARG0)) & 0x1FFFFFFFu;
    gpr data = ARG1;
    std::span<const uint8_t> rom = recomp::get_rom();
    const uint32_t offset = dev_addr - 0x10000000u;
    uint32_t value = 0;
    if (dev_addr >= 0x10000000u && offset + 4 <= rom.size()) {
        value = (uint32_t(rom[offset]) << 24) | (uint32_t(rom[offset + 1]) << 16) | (uint32_t(rom[offset + 2]) << 8) | rom[offset + 3];
    }
    MEM_W(0, data) = static_cast<int32_t>(value);
    RET(0);
}

void osPiReadIo_recomp(uint8_t* rdram, recomp_context* ctx) { osPiRawReadIo_recomp(rdram, ctx); }

// Rare's small-transfer PI DMA (src/main/init_4470.c): (u32 romOffset, void* dst, u32 size).
// Polls PI_STATUS and writes the PI registers on hardware; here the copy happens directly.
void func_8000480C(uint8_t* rdram, recomp_context* ctx) {
    uint32_t source = as_u32(ARG0);
    const gpr dst = ARG1;
    const uint32_t size = as_u32(ARG2);
    if (source < 0x10000000u) {
        source += 0x10000000u;  // ROM offset -> cartridge physical address
    }
    source &= 0x1FFFFFFFu;
    std::span<const uint8_t> rom = recomp::get_rom();
    const uint32_t offset = source - 0x10000000u;
    for (uint32_t i = 0; i < size; i++) {
        MEM_B(i, dst) = static_cast<int8_t>(offset + i < rom.size() ? rom[offset + i] : 0);
    }
}

// libultra's osContInit (main copy 0x10025340, game-overlay copy 0x151EF090; see CONKER_RENAMES in
// tools/gen_syms.py). Both use the same globals. The runtime's osContInit does the controller
// part; around it go the steps it leaves out: the __osContinitialized guard and, last, creating
// __osEepromTimerQ with __osEepromTimerMsg as its one-message buffer. Rare's paced EEPROM write
// (func_151DCEF0, __conker_eeprom_wait) arms its 12 ms timer on that queue and waits on it.
constexpr uint32_t kOsContInitialized = 0x8002BE10;
constexpr uint32_t kOsEepromTimerQ = 0x80042A78;
constexpr uint32_t kOsEepromTimerMsg = 0x80042A90;

void osContInit_recomp(uint8_t* rdram, recomp_context* ctx);

void conker_osContInit(uint8_t* rdram, recomp_context* ctx) {
    const gpr initialized = static_cast<gpr>(static_cast<int32_t>(kOsContInitialized));
    if (MEM_W(0, initialized) != 0) {
        RET(0);
        return;
    }
    MEM_W(0, initialized) = 1;
    osContInit_recomp(rdram, ctx);
    const gpr ret = ctx->r2;
    osCreateMesgQueue(rdram, static_cast<int32_t>(kOsEepromTimerQ), static_cast<int32_t>(kOsEepromTimerMsg), 1);
    ctx->r2 = ret;
}

void conker_osContInit_game(uint8_t* rdram, recomp_context* ctx) { conker_osContInit(rdram, ctx); }

// --- SI / PIF ---------------------------------------------------------------------------------

void __osSiCreateAccessQueue_recomp(uint8_t* /*rdram*/, recomp_context* /*ctx*/) {}
void __osSiGetAccess_recomp(uint8_t* /*rdram*/, recomp_context* /*ctx*/) {}
void __osSiRelAccess_recomp(uint8_t* /*rdram*/, recomp_context* /*ctx*/) {}
void __osSiDeviceBusy_recomp(uint8_t* /*rdram*/, recomp_context* ctx) { RET(0); }

void __osSiRawStartDma_recomp(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t direction = as_u32(ARG0);
    gpr dram_addr = ARG1;
    std::lock_guard lock{pif_mutex};
    if (direction == 1 /*OS_WRITE*/) {
        for (uint32_t i = 0; i < sizeof(pif_ram); i++) {
            pif_ram[i] = static_cast<uint8_t>(MEM_B(i, dram_addr));
        }
        pif_process_commands();
    } else {
        for (uint32_t i = 0; i < sizeof(pif_ram); i++) {
            MEM_B(i, dram_addr) = static_cast<int8_t>(pif_ram[i]);
        }
    }
    RET(0);
}

void __osEepStatus_recomp(uint8_t* rdram, recomp_context* ctx) {
    // (OSMesgQueue* mq, OSContStatus* status): report a 16 kbit EEPROM.
    gpr status = ARG1;
    MEM_H(0, status) = static_cast<int16_t>(0xC000);
    MEM_B(2, status) = 0;
    MEM_B(3, status) = 0;
    RET(0);
}

void __osContGetInitData_recomp(uint8_t* rdram, recomp_context* ctx) {
    // (u8* pattern, OSContStatus* data[4]): a standard controller in each connected port, with a
    // pak (status CONT_CARD_ON) when its host pad can rumble.
    gpr pattern = ARG0, data = ARG1;
    uint8_t bits = 0;
    for (int i = 0; i < 4; i++) {
        const bool present = conker::frame_input::connected(i);
        bits |= present ? (1 << i) : 0;
        MEM_H(i * 4 + 0, data) = static_cast<int16_t>(present ? 0x0500 : 0);
        MEM_B(i * 4 + 2, data) = static_cast<int8_t>(present && conker::frame_input::has_rumble(i) ? 1 : 0);
        MEM_B(i * 4 + 3, data) = static_cast<int8_t>(present ? 0 : 0x80);
    }
    MEM_B(0, pattern) = static_cast<int8_t>(bits);
}

void __osContAddressCrc_recomp(uint8_t* /*rdram*/, recomp_context* ctx) {
    uint32_t addr = as_u32(ARG0) & 0xFFFF, crc = 0;
    for (int i = 0; i < 10; i++) {
        crc <<= 1;
        if (addr & 0x200) crc |= 1;
        addr <<= 1;
        if (crc & 0x20) crc ^= 0x15;
    }
    for (int i = 0; i < 5; i++) {
        crc <<= 1;
        if (crc & 0x20) crc ^= 0x15;
    }
    RET(crc & 0x1F);
}

void __osContDataCrc_recomp(uint8_t* rdram, recomp_context* ctx) {
    gpr data = ARG0;
    uint32_t crc = 0;
    for (uint32_t i = 0; i < 32; i++) {
        const uint8_t byte = static_cast<uint8_t>(MEM_B(i, data));
        for (int bit = 7; bit >= 0; bit--) {
            crc <<= 1;
            if (byte & (1 << bit)) crc |= 1;
            if (crc & 0x100) crc ^= 0x85;
        }
    }
    for (int i = 0; i < 8; i++) {
        crc <<= 1;
        if (crc & 0x100) crc ^= 0x85;
    }
    RET(crc & 0xFF);
}

// --- Rumble pak -----------------------------------------------------------------------------
// Rare's motor routines (src/main/init_57E0.c, src/main/init_5570.c) work on a 0x68-byte device
// record: status at 0x0, message queue at 0x4, channel at 0x8, active bank at 0x65. A controller
// has a rumble pak whenever its host pad can rumble (conker::input::has_rumble); the game probes
// for it at boot (func_15006234: pak status bit, then osPfsInit, then func_800057E0) and again
// from func_1501C1B0, and keeps the result per port at 0x800BE944.

constexpr int32_t PFS_ERR_NOPACK = 1;
constexpr int32_t PFS_ERR_CONTRFAIL = 4;
constexpr int32_t PFS_ERR_DEVICE = 11;

static int32_t rumble_pak_status(int32_t channel) {
    if (channel < 0 || channel >= 4 || !conker::frame_input::connected(channel)) {
        return PFS_ERR_CONTRFAIL;  // no controller in that port
    }
    return conker::frame_input::has_rumble(channel) ? 0 : PFS_ERR_NOPACK;
}

// Motor init: (OSMesgQueue* queue, device*, int channel). On hardware it selects bank 0x80, reads
// it back to tell a rumble pak from a controller pak, then prebuilds the stop and start packets.
void func_800057E0(uint8_t* rdram, recomp_context* ctx) {
    const gpr device = ARG1;
    const int32_t channel = static_cast<int32_t>(ARG2);
    MEM_W(0x4, device) = static_cast<int32_t>(ARG0);
    MEM_W(0x8, device) = channel;
    MEM_W(0x0, device) = 0;
    MEM_B(0x65, device) = static_cast<int8_t>(0x80);
    const int32_t status = rumble_pak_status(channel);
    static std::atomic<int32_t> reported[4] = {-1, -1, -1, -1};
    if (channel >= 0 && channel < 4 && status != PFS_ERR_CONTRFAIL && reported[channel].exchange(status) != status) {
        std::printf("[conker] controller %d rumble pak %s\n", channel + 1,
                    status == 0 ? "inserted (host pad rumble)" : "not inserted (no pad that can rumble)");
    }
    RET(status);
}

// Logs each change of the motor the game asks for. The game pulses the motor on and off to set
// its strength, so a rumble shows up as a run of on/off lines.
static void motor_access(uint8_t* rdram, recomp_context* ctx, bool on) {
    const int32_t channel = MEM_W(0x8, ARG0);
    const int32_t status = rumble_pak_status(channel);
    static std::atomic<int> last[4] = {-1, -1, -1, -1};
    const int state = (on ? 1 : 0) | (status == 0 ? 0 : 2);
    if (channel >= 0 && channel < 4 && last[channel].exchange(state) != state) {
        std::printf("[conker] VI %llu: rumble %s, port %d%s\n", static_cast<unsigned long long>(conker::current_vi()),
                    on ? "on" : "off", channel + 1, status == 0 ? "" : " (no pak, ignored)");
    }
    // Online, only the port this machine's controller holds rumbles here.
    if (status == 0 && conker::net::local_controller(channel) >= 0) {
        conker::input::set_rumble(conker::net::local_controller(channel), on);
    }
    RET(status);
}

void func_80005570(uint8_t* rdram, recomp_context* ctx) { motor_access(rdram, ctx, false); }  // motor stop
void func_800056A0(uint8_t* rdram, recomp_context* ctx) { motor_access(rdram, ctx, true); }   // motor start

// --- Controller pak: report "no pak" everywhere ------------------------------------------------

// (OSMesgQueue* queue, OSPfs* pfs, int channel). With a rumble pak inserted, libultra fails to read
// a controller pak ID and returns PFS_ERR_DEVICE; the game's boot probe (func_15006234) only tries
// the motor after that answer, from a port whose status says a pak is inserted.
void osPfsInit_recomp(uint8_t* /*rdram*/, recomp_context* ctx) {
    report_once("osPfsInit", "no controller pak");
    RET(rumble_pak_status(static_cast<int32_t>(ARG2)) == 0 ? PFS_ERR_DEVICE : PFS_ERR_NOPACK);
}
void osPfsIsPlug_recomp(uint8_t* rdram, recomp_context* ctx) { MEM_B(0, ARG1) = 0; RET(0); }
void osPfsChecker_recomp(uint8_t* /*rdram*/, recomp_context* ctx) { RET(PFS_ERR_NOPACK); }
void __osContRamRead_recomp(uint8_t* /*rdram*/, recomp_context* ctx) { RET(PFS_ERR_NOPACK); }
void __osContRamWrite_recomp(uint8_t* /*rdram*/, recomp_context* ctx) { RET(PFS_ERR_NOPACK); }
void __osPfsSelectBank_recomp(uint8_t* /*rdram*/, recomp_context* ctx) { RET(PFS_ERR_NOPACK); }
void __osPfsRWInode_recomp(uint8_t* /*rdram*/, recomp_context* ctx) { RET(PFS_ERR_NOPACK); }
void __osPfsGetStatus_recomp(uint8_t* /*rdram*/, recomp_context* ctx) { RET(PFS_ERR_NOPACK); }
void __osPfsRequestData_recomp(uint8_t* /*rdram*/, recomp_context* ctx) { RET(PFS_ERR_NOPACK); }
void __osPfsGetInitData_recomp(uint8_t* rdram, recomp_context* ctx) { MEM_B(0, ARG0) = 0; RET(PFS_ERR_NOPACK); }
void __osPfsRequestOneChannel_recomp(uint8_t* /*rdram*/, recomp_context* ctx) { RET(PFS_ERR_NOPACK); }
void __osPfsGetOneChannelData_recomp(uint8_t* /*rdram*/, recomp_context* ctx) { RET(PFS_ERR_NOPACK); }
void __osPackRequestData_recomp(uint8_t* /*rdram*/, recomp_context* /*ctx*/) {}
void __osGetId_recomp(uint8_t* /*rdram*/, recomp_context* ctx) { RET(PFS_ERR_NOPACK); }
void __osCheckId_recomp(uint8_t* /*rdram*/, recomp_context* ctx) { RET(PFS_ERR_NOPACK); }
void __osRepairPackId_recomp(uint8_t* /*rdram*/, recomp_context* ctx) { RET(PFS_ERR_NOPACK); }
void __osCheckPackId_recomp(uint8_t* /*rdram*/, recomp_context* ctx) { RET(PFS_ERR_NOPACK); }
void corrupted_init_recomp(uint8_t* /*rdram*/, recomp_context* ctx) { RET(0); }
void corrupted_recomp(uint8_t* /*rdram*/, recomp_context* ctx) { RET(0); }

void recomp_syscall_handler(uint8_t* rdram, recomp_context* ctx, int32_t instruction_vram) {
    // Conker executes `syscall` in its audio fault handler and once in the MP3 code. Nothing in
    // the runtime services it; report and continue.
    report_once("recomp_syscall_handler", "syscall instruction executed; ignored");
    (void)instruction_vram;
    (void)rdram; (void)ctx;
}

} // extern "C"
