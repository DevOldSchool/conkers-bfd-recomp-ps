// Headless runtime for the Conker's Bad Fur Day recompilation.
//
//   ConkersBadFurDayRecompiled [path to US ROM .z64]
//
// The ROM is kept in conker-recomp-data once chosen, so later runs need no argument. The packaged
// RT64 build asks for it with a dialog instead (src/platform/launch.hpp).
// Registers the game with librecomp, wires the ultramodern callbacks and starts the game thread.
// Controller 1 is an SDL game controller or the keyboard in the RT64 build
// (src/platform/input_sdl.cpp), plus any scripted Start or A presses (CONKER_PRESS_START,
// CONKER_PRESS_A). Graphics depend on CONKER_RENDERER: the null renderer (default) runs headless
// and only logs graphics tasks, RT64 opens a window and draws the display lists through Conker's
// F3DEX2CBFD microcode support. Audio is rendered by the recompiled audio microcode and played
// through SDL2 in the RT64 build (src/platform/audio.cpp).
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <execinfo.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#else
static long _dyld_get_image_vmaddr_slide(int) { return 0; }
#endif

#include "runtime/memory.hpp"

#include "librecomp/addresses.hpp"
#include "librecomp/game.hpp"
#include "ultramodern/ultramodern.hpp"

#include "platform/audio.hpp"
#include "platform/input.hpp"
#include "platform/window.hpp"
#include "runtime/frame_input.hpp"
#include "runtime/game.hpp"
#include "runtime/net_play.hpp"
#include "runtime/rsp.hpp"
#if CONKER_LAUNCH_UI
#include "platform/launch.hpp"
#endif
#if CONKER_RENDERER_RT64
#include "platform/renderer_rt64.hpp"
#include <SDL.h>
#else
#include "platform/renderer_null.hpp"
#endif

namespace conker::trace { void dump_recent_calls(int fd); }

namespace {

// --- input ---------------------------------------------------------------------------------
constexpr uint64_t kPressLength = 10;  // VIs

std::atomic<uint64_t> vi_count{0};
struct ScriptedPress {
    uint64_t vi;
    uint16_t button;
    const char* name;
};
std::vector<ScriptedPress> scripted_presses;  // from CONKER_PRESS_START and CONKER_PRESS_A

void parse_scripted_presses(const char* variable, uint16_t button, const char* name) {
    const char* spec = std::getenv(variable);
    for (const char* p = spec; p != nullptr && *p != '\0';) {
        char* end;
        const unsigned long long vi = std::strtoull(p, &end, 0);
        if (end == p) break;
        scripted_presses.push_back({vi, button, name});
        p = (*end == ',') ? end + 1 : end;
    }
}

void parse_scripted_input() {
    parse_scripted_presses("CONKER_PRESS_START", conker::input::kStart, "Start");
    parse_scripted_presses("CONKER_PRESS_A", conker::input::kA, "A");
}

void poll_input() {}

bool get_input(int controller_num, uint16_t* buttons, float* x, float* y) {
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) {
        std::printf("[conker] controllers read through osContGetReadData (VI %llu)\n",
                    static_cast<unsigned long long>(vi_count.load()));
    }
    conker::frame_input::read(buttons, x, y, controller_num);
    return conker::frame_input::connected(controller_num);
}

void set_rumble(int controller_num, bool rumble) {
    // Online, only the port this machine's controller holds rumbles here.
    const int controller = conker::net::local_controller(controller_num);
    if (controller >= 0) {
        conker::input::set_rumble(controller, rumble);
    }
}

ultramodern::input::connected_device_info_t get_connected_device_info(int controller_num) {
    if (!conker::frame_input::connected(controller_num)) {
        return {ultramodern::input::Device::None, ultramodern::input::Pak::None};
    }
    return {
        ultramodern::input::Device::Controller,
        conker::frame_input::has_rumble(controller_num) ? ultramodern::input::Pak::RumblePak : ultramodern::input::Pak::None,
    };
}

// --- misc ----------------------------------------------------------------------------------
void message_box(const char* msg) {
    std::fprintf(stderr, "[conker] %s\n", msg);
#if CONKER_LAUNCH_UI
    // Started from the desktop, nobody reads the terminal.
    if (conker::launch::from_desktop()) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Conker's Bad Fur Day Recompiled", msg, nullptr);
    }
#endif
}
void vi_callback() { vi_count++; }
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
    } else if (rdram != nullptr && (fault - (reinterpret_cast<uintptr_t>(rdram) - recomp::rdram_guard_size) < recomp::rdram_guard_size ||
                                    fault - (reinterpret_cast<uintptr_t>(rdram) + recomp::allocation_size) < recomp::rdram_guard_size)) {
        // A pointer from game memory plus an instruction's offset ran past either end of the 4 GiB: the
        // guard around it stopped the access.
        const char msg[] = " = just outside game memory (the guard around RDRAM); stopping";
        write(2, msg, sizeof(msg) - 1);
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

uint64_t conker::current_vi() { return vi_count.load(); }

uint16_t conker::scripted_buttons() {
    const uint64_t now = vi_count.load();
    uint16_t buttons = 0;
    for (const ScriptedPress& press : scripted_presses) {
        if (now >= press.vi && now < press.vi + kPressLength) {
            static std::atomic<const ScriptedPress*> last_reported{nullptr};
            if (last_reported.exchange(&press) != &press) {
                std::printf("[conker] scripted input: %s held from VI %llu\n", press.name,
                            static_cast<unsigned long long>(now));
            }
            buttons |= press.button;
        }
    }
    return buttons;
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // keep log order intact if the game thread crashes
    install_crash_handler();
    parse_scripted_input();
    if (!conker::net::parse_args(&argc, argv)) {
        return 2;
    }
#if CONKER_LAUNCH_UI
    conker::launch::enter_data_folder();
#endif

    // Stored ROM copy, saves and mod folders live here.
    const std::filesystem::path data_dir = std::filesystem::current_path() / "conker-recomp-data";
    std::filesystem::create_directories(data_dir);
    recomp::register_config_path(data_dir);
    recomp::register_game(conker::make_game_entry());
    conker::register_overlays();

    // A ROM given on the command line replaces the stored copy; without one the stored copy is used.
    std::filesystem::path rom_path = argc >= 2 ? std::filesystem::path(argv[1]) : std::filesystem::path();
    if (rom_path.empty()) {
        recomp::check_all_stored_roms();
    }
    if (rom_path.empty() && !recomp::is_rom_valid(conker::kGameId)) {
#if CONKER_LAUNCH_UI
#if defined(__APPLE__)
        if (SDL_InitSubSystem(SDL_INIT_VIDEO) == 0)
#endif
        {
            rom_path = conker::launch::choose_rom();
        }
#endif
        if (rom_path.empty()) {
#if CONKER_LAUNCH_UI
            if (conker::launch::from_desktop()) {
                message_box("No ROM was chosen. Open the game again and choose your own Conker's Bad Fur Day "
                            "(US) ROM to play.");
            }
#endif
            std::fprintf(stderr, "usage: %s <conker-us.z64>\n", argv[0]);
            return 2;
        }
    }
    if (!rom_path.empty()) {
        const recomp::RomValidationError rom_result = recomp::select_rom(rom_path, conker::kGameId);
        if (rom_result != recomp::RomValidationError::Good) {
            const std::string error = "ROM " + rom_path.string() + ": " + rom_error_name(rom_result);
            message_box(error.c_str());
            return 1;
        }
    }

    // Online: every player is here and agreed before the game starts.
    if (!conker::net::connect()) {
        return 1;
    }

    recomp::Configuration cfg{};
    cfg.argc = argc;
    cfg.argv = argv;
    cfg.project_version = recomp::Version{0, 1, 0, "-dev"};
    cfg.rsp_callbacks = {.get_rsp_microcode = conker::rsp::get_rsp_microcode};
#if CONKER_RENDERER_RT64
    cfg.renderer_callbacks = {.create_render_context = conker::renderer::create_rt64_render_context};
#else
    cfg.renderer_callbacks = {.create_render_context = conker::renderer::create_null_render_context};
#endif
    conker::audio::init();
    cfg.audio_callbacks = {
        .queue_samples = conker::audio::queue_samples,
        .get_frames_remaining = conker::audio::get_frames_remaining,
        .set_frequency = conker::audio::set_frequency,
    };
    cfg.input_callbacks = {
        .poll_input = poll_input,
        .get_input = get_input,
        .set_rumble = set_rumble,
        .get_connected_device_info = get_connected_device_info,
    };
    cfg.gfx_callbacks = {
        .create_gfx = conker::window::create_gfx,
        .create_window = conker::window::create_window,
        .update_gfx = conker::window::update_gfx,
    };
    cfg.events_callbacks = {.vi_callback = vi_callback, .gfx_init_callback = gfx_init_callback};
    cfg.error_handling_callbacks = {.message_box = message_box};
    cfg.threads_callbacks = {.get_game_thread_name = nullptr};

    recomp::start_game(conker::kGameId, "");
    recomp::start(cfg);
    return 0;
}
