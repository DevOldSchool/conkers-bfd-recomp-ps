// Headless runtime for the Conker's Bad Fur Day recompilation.
//
//   ConkerRecomp <path to US ROM .z64>
//
// Registers the game with librecomp, wires every ultramodern callback to a do-nothing
// implementation (no window, no audio output, no input) and starts the game thread. The point
// of this binary today is to exercise the recompiled code; graphics arrive with RT64 support
// for Conker's microcode.
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include <execinfo.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#else
static long _dyld_get_image_vmaddr_slide(int) { return 0; }
#endif

#include "memory.hpp"

#include "librecomp/game.hpp"
#include "ultramodern/ultramodern.hpp"

#include "game.hpp"
#include "renderer_null.hpp"
#include "rsp.hpp"

namespace conker::trace { void dump_recent_calls(int fd); }

namespace {

// --- input ---------------------------------------------------------------------------------
void poll_input() {}

bool get_input(int /*controller_num*/, uint16_t* buttons, float* x, float* y) {
    *buttons = 0;
    *x = 0.0f;
    *y = 0.0f;
    return true;
}

void set_rumble(int /*controller_num*/, bool /*rumble*/) {}

ultramodern::input::connected_device_info_t get_connected_device_info(int controller_num) {
    return {
        controller_num == 0 ? ultramodern::input::Device::Controller : ultramodern::input::Device::None,
        ultramodern::input::Pak::None,
    };
}

// --- audio ---------------------------------------------------------------------------------
void queue_samples(int16_t* /*samples*/, size_t /*count*/) {}
size_t get_frames_remaining() { return 0; }
void set_frequency(uint32_t /*freq*/) {}

// --- window / gfx loop ---------------------------------------------------------------------
void* create_gfx() { return nullptr; }
ultramodern::renderer::WindowHandle create_window(void* /*gfx_data*/) { return {}; }
void update_gfx(void* /*gfx_data*/) {}

// --- misc ----------------------------------------------------------------------------------
void message_box(const char* msg) { std::fprintf(stderr, "[conker] %s\n", msg); }
void vi_callback() {}
void gfx_init_callback() { std::printf("[conker] gfx thread started\n"); }

// Crash reporter: prints the faulting address as an N64 address when it lies inside rdram, plus
// a host backtrace. The recompiled functions are named after the decomp's symbols, so the
// backtrace reads like an N64 call stack.
void crash_handler(int sig, siginfo_t* info, void* /*uctx*/) {
    char buf[256];
    const uint8_t* rdram = conker::memory::current_rdram();
    const uintptr_t fault = reinterpret_cast<uintptr_t>(info->si_addr);
    int len = std::snprintf(buf, sizeof(buf), "\n[conker] fatal signal %d at host address %p", sig, info->si_addr);
    write(2, buf, len);
    if (rdram != nullptr && fault >= reinterpret_cast<uintptr_t>(rdram) && fault < reinterpret_cast<uintptr_t>(rdram) + 0x100000000ull) {
        const uint64_t offset = fault - reinterpret_cast<uintptr_t>(rdram);
        const uint32_t n64_addr = static_cast<uint32_t>(offset + 0xFFFFFFFF80000000ull);
        len = std::snprintf(buf, sizeof(buf), " = N64 address 0x%08X (rdram + 0x%llX)", n64_addr, static_cast<unsigned long long>(offset));
        write(2, buf, len);
    }
    write(2, "\n", 1);
    if (recomp_context* ctx = conker::current_context()) {
        static const char* const names[32] = {"zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
                                              "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"};
        const gpr* regs = &ctx->r0;
        for (int i = 0; i < 32; i++) {
            len = std::snprintf(buf, sizeof(buf), "  %-4s %08X%s", names[i], static_cast<uint32_t>(regs[i]), (i % 4 == 3) ? "\n" : "");
            write(2, buf, len);
        }
    }
    if (rdram != nullptr) {
        // Peek at the inflated game data image so a failed startup inflate is visible.
        len = std::snprintf(buf, sizeof(buf), "  game data @0x80082B20:");
        write(2, buf, len);
        for (uint32_t i = 0; i < 16; i++) {
            len = std::snprintf(buf, sizeof(buf), " %02X", rdram[conker::memory::rdram_offset(0x80082B20 + i) ^ 3]);
            write(2, buf, len);
        }
        write(2, "\n", 1);
    }
    conker::trace::dump_recent_calls(2);
    len = std::snprintf(buf, sizeof(buf), "[conker] image slide 0x%llx (tools/symbolicate.sh)\n", static_cast<unsigned long long>(_dyld_get_image_vmaddr_slide(0)));
    write(2, buf, len);
    void* frames[64];
    const int count = backtrace(frames, 64);
    backtrace_symbols_fd(frames, count, 2);
    _exit(128 + sig);
}

// librecomp calls std::exit on fatal runtime errors (e.g. a function-pointer call to an address
// with no function). Show the calling thread's recent recompiled calls on the way out.
void dump_trace_at_exit() {
    if (conker::current_context() != nullptr) {
        conker::trace::dump_recent_calls(2);
    }
}

void install_crash_handler() {
    std::atexit(dump_trace_at_exit);
    struct sigaction action{};
    action.sa_sigaction = crash_handler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    sigaction(SIGSEGV, &action, nullptr);
    sigaction(SIGBUS, &action, nullptr);
    sigaction(SIGILL, &action, nullptr);
    sigaction(SIGFPE, &action, nullptr);
}

const char* rom_error_name(recomp::RomValidationError err) {
    switch (err) {
        case recomp::RomValidationError::Good: return "ok";
        case recomp::RomValidationError::FailedToOpen: return "failed to open";
        case recomp::RomValidationError::NotARom: return "not an N64 ROM";
        case recomp::RomValidationError::IncorrectRom: return "not Conker's Bad Fur Day (US)";
        case recomp::RomValidationError::NotYet: return "not supported yet";
        case recomp::RomValidationError::IncorrectVersion: return "wrong version";
        default: return "unknown error";
    }
}

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // keep log order intact if the game thread crashes
    install_crash_handler();
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <conker-us.z64>\n", argv[0]);
        return 2;
    }

    // Stored ROM copy, saves and mod folders live here.
    const std::filesystem::path data_dir = std::filesystem::current_path() / "conker-recomp-data";
    std::filesystem::create_directories(data_dir);
    recomp::register_config_path(data_dir);
    recomp::register_game(conker::make_game_entry());
    conker::register_overlays();

    const recomp::RomValidationError rom_result = recomp::select_rom(argv[1], conker::kGameId);
    if (rom_result != recomp::RomValidationError::Good) {
        std::fprintf(stderr, "[conker] ROM %s: %s\n", argv[1], rom_error_name(rom_result));
        return 1;
    }

    recomp::Configuration cfg{};
    cfg.argc = argc;
    cfg.argv = argv;
    cfg.project_version = recomp::Version{0, 1, 0, "-dev"};
    cfg.rsp_callbacks = {.get_rsp_microcode = conker::rsp::get_rsp_microcode};
    cfg.renderer_callbacks = {.create_render_context = conker::renderer::create_null_render_context};
    cfg.audio_callbacks = {.queue_samples = queue_samples, .get_frames_remaining = get_frames_remaining, .set_frequency = set_frequency};
    cfg.input_callbacks = {
        .poll_input = poll_input,
        .get_input = get_input,
        .set_rumble = set_rumble,
        .get_connected_device_info = get_connected_device_info,
    };
    cfg.gfx_callbacks = {.create_gfx = create_gfx, .create_window = create_window, .update_gfx = update_gfx};
    cfg.events_callbacks = {.vi_callback = vi_callback, .gfx_init_callback = gfx_init_callback};
    cfg.error_handling_callbacks = {.message_box = message_box};
    cfg.threads_callbacks = {.get_game_thread_name = nullptr};

    recomp::start_game(conker::kGameId, "");
    recomp::start(cfg);
    return 0;
}
