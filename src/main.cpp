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
#include <cctype>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <dbghelp.h>
#include <io.h>
#elif CONKER_PS5
#include <unistd.h>
#include "platform/ps5_platform.hpp"
#else
#include <execinfo.h>
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif !defined(_WIN32)
static long _dyld_get_image_vmaddr_slide(int) { return 0; }
#endif

#include "runtime/memory.hpp"
#include "platform/data_folder.hpp"

#include "librecomp/addresses.hpp"
#include "librecomp/game.hpp"
#include "ultramodern/ultramodern.hpp"

#include "platform/audio.hpp"
#include "platform/input.hpp"
#include "platform/input_config.hpp"
#include "platform/window.hpp"
#include "runtime/frame_input.hpp"
#include "runtime/game.hpp"
#include "net/net_play.hpp"
#include "runtime/rsp.hpp"
#if CONKER_LAUNCH_UI
#include "platform/crash_log.hpp"
#include "platform/launch.hpp"
#endif
#include <exception>
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
#if defined(_WIN32)
LONG WINAPI crash_filter(EXCEPTION_POINTERS* info);
#endif
void gfx_init_callback() {
    std::printf("[conker] gfx thread started\n");
#if defined(_WIN32)
    // A graphics driver can put in its own unhandled-exception filter while the renderer starts;
    // a crash after that would then leave no report. Ours goes back on top.
    LPTOP_LEVEL_EXCEPTION_FILTER previous = SetUnhandledExceptionFilter(crash_filter);
    if (previous != crash_filter) {
        std::printf("[conker] crash filter was replaced while the renderer started; put back\n");
    }
#endif
}

#if defined(_WIN32)
void write_stderr(const char* data, int size) { _write(2, data, static_cast<unsigned>(size)); }
#else
void write_stderr(const char* data, int size) { write(2, data, static_cast<size_t>(size)); }
#endif

// Crash reporter: prints the faulting address as an N64 address when it lies inside rdram, plus
// a host backtrace. The recompiled functions are named after the decomp's symbols, so the
// backtrace reads like an N64 call stack.
void report_crash(const char* what, void* address) {
    char buf[256];
    const uint8_t* rdram = conker::memory::current_rdram();
    const uintptr_t fault = reinterpret_cast<uintptr_t>(address);
    int len = std::snprintf(buf, sizeof(buf), "\n[conker] %s at host address %p", what, address);
    write_stderr(buf, len);
    if (rdram != nullptr && fault >= reinterpret_cast<uintptr_t>(rdram) && fault < reinterpret_cast<uintptr_t>(rdram) + 0x100000000ull) {
        const uint64_t offset = fault - reinterpret_cast<uintptr_t>(rdram);
        const uint32_t n64_addr = static_cast<uint32_t>(offset + 0xFFFFFFFF80000000ull);
        len = std::snprintf(buf, sizeof(buf), " = N64 address 0x%08X (rdram + 0x%llX)", n64_addr, static_cast<unsigned long long>(offset));
        write_stderr(buf, len);
    } else if (rdram != nullptr && (fault - (reinterpret_cast<uintptr_t>(rdram) - recomp::rdram_guard_size) < recomp::rdram_guard_size ||
                                    fault - (reinterpret_cast<uintptr_t>(rdram) + recomp::allocation_size) < recomp::rdram_guard_size)) {
        // A pointer from game memory plus an instruction's offset ran past either end of the 4 GiB: the
        // guard around it stopped the access.
        const char msg[] = " = just outside game memory (the guard around RDRAM); stopping";
        write_stderr(msg, sizeof(msg) - 1);
    }
    write_stderr("\n", 1);
    if (recomp_context* ctx = conker::current_context()) {
        static const char* const names[32] = {"zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
                                              "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"};
        const gpr* regs = &ctx->r0;
        for (int i = 0; i < 32; i++) {
            len = std::snprintf(buf, sizeof(buf), "  %-4s %08X%s", names[i], static_cast<uint32_t>(regs[i]), (i % 4 == 3) ? "\n" : "");
            write_stderr(buf, len);
        }
    }
    if (rdram != nullptr) {
        // Peek at the inflated game data image so a failed startup inflate is visible.
        len = std::snprintf(buf, sizeof(buf), "  game data @0x80082B20:");
        write_stderr(buf, len);
        for (uint32_t i = 0; i < 16; i++) {
            len = std::snprintf(buf, sizeof(buf), " %02X", rdram[conker::memory::rdram_offset(0x80082B20 + i) ^ 3]);
            write_stderr(buf, len);
        }
        write_stderr("\n", 1);
    }
    conker::trace::dump_recent_calls(2);
}

#if defined(_WIN32)
// Names each frame from the PDB next to the executable when there is one.
void print_backtrace(CONTEXT* context) {
    // Windows' own unwinder (the function tables in each module), which follows ARM64's signed return
    // addresses where StackWalk64 stopped after a frame or two.
    HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    const bool symbols = SymInitialize(process, nullptr, TRUE);
    CONTEXT ctx = *context;
    alignas(SYMBOL_INFO) char symbol_buf[sizeof(SYMBOL_INFO) + 256];
    for (int i = 0; i < 64; i++) {
#if defined(_M_ARM64)
        const DWORD64 pc = ctx.Pc & 0x0000FFFFFFFFFFFFULL;  // without the pointer-authentication bits
#else
        const DWORD64 pc = ctx.Rip;
#endif
        if (pc == 0) {
            break;
        }
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(symbol_buf);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 255;
        DWORD64 displacement = 0;
        char module[MAX_PATH] = "?";
        HMODULE handle = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(pc), &handle)) {
            char path[MAX_PATH];
            if (GetModuleFileNameA(handle, path, MAX_PATH) != 0) {
                const char* slash = std::strrchr(path, '\\');
                std::snprintf(module, sizeof(module), "%s", slash != nullptr ? slash + 1 : path);
            }
        }
        const unsigned long long offset = handle != nullptr ? pc - reinterpret_cast<DWORD64>(handle) : 0;
        char buf[512];
        int len;
        if (symbols && SymFromAddr(process, pc, &displacement, symbol)) {
            len = std::snprintf(buf, sizeof(buf), "%-3d %s+0x%llx %s + %llu\n", i, module, offset, symbol->Name,
                                static_cast<unsigned long long>(displacement));
        } else {
            len = std::snprintf(buf, sizeof(buf), "%-3d %s+0x%llx\n", i, module, offset);
        }
        write_stderr(buf, len);

        DWORD64 image_base = 0;
        PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(pc, &image_base, nullptr);
        if (function == nullptr) {
            // A leaf function: the return address is in LR (ARM64) or on top of the stack (x64).
#if defined(_M_ARM64)
            if (ctx.Lr == 0 || (ctx.Lr & 0x0000FFFFFFFFFFFFULL) == pc) {
                break;
            }
            ctx.Pc = ctx.Lr;
            ctx.Lr = 0;
#else
            ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
            ctx.Rsp += 8;
#endif
            continue;
        }
        PVOID handler_data = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, pc, function, &ctx, &handler_data, &establisher, nullptr);
    }
}

// The message of the last C++ exception thrown on each thread, noted as it is thrown: one nobody
// catches ends the process through std::terminate, where MSVC no longer has it as current.
thread_local char t_last_throw[512];

LONG CALLBACK note_cxx_throw(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* record = info->ExceptionRecord;
    // MSVC's C++ throw: code 'msc', magic, the object, its ThrowInfo, and the image the RVAs are from.
    if (record->ExceptionCode != 0xE06D7363 || record->NumberParameters < 4 ||
        record->ExceptionInformation[0] != 0x19930520 || record->ExceptionInformation[2] == 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    auto* object = reinterpret_cast<char*>(record->ExceptionInformation[1]);
    const auto* throw_info = reinterpret_cast<const uint8_t*>(record->ExceptionInformation[2]);
    const uintptr_t base = record->ExceptionInformation[3];
    // ThrowInfo: attributes, unwind, forward-compat, catchable type array (each a 32-bit field).
    const auto* types = reinterpret_cast<const int32_t*>(base + *reinterpret_cast<const int32_t*>(throw_info + 12));
    const char* first_name = nullptr;
    for (int32_t i = 0; i < types[0] && i < 16; i++) {
        // CatchableType: properties, type descriptor, this displacement (mdisp, pdisp, vdisp), size, copy.
        const auto* type = reinterpret_cast<const uint8_t*>(base + types[1 + i]);
        const auto* descriptor = reinterpret_cast<const uint8_t*>(base + *reinterpret_cast<const int32_t*>(type + 4));
        const char* name = reinterpret_cast<const char*>(descriptor + 2 * sizeof(void*));
        if (first_name == nullptr) {
            first_name = name;
        }
        if (std::strcmp(name, ".?AVexception@std@@") == 0) {
            const int32_t displacement = *reinterpret_cast<const int32_t*>(type + 8);
            const auto* e = reinterpret_cast<const std::exception*>(object + displacement);
            std::snprintf(t_last_throw, sizeof(t_last_throw), "%s: %s", first_name, e->what());
            return EXCEPTION_CONTINUE_SEARCH;
        }
    }
    std::snprintf(t_last_throw, sizeof(t_last_throw), "%s", first_name != nullptr ? first_name : "unknown type");
    return EXCEPTION_CONTINUE_SEARCH;
}

// Several threads can fail at once (the renderer's workers did): the first one writes the report and
// ends the process; the others wait for it rather than write over its minidump.
std::atomic_flag g_reporting = ATOMIC_FLAG_INIT;
void claim_report() {
    if (g_reporting.test_and_set()) {
        Sleep(INFINITE);
    }
}

// Where a crash's minidump goes: crash-reports/pending.dmp, renamed with its log at the next start
// (crash_log.cpp). Empty when started from a terminal. Set before any crash, so the filter builds nothing.
wchar_t g_dump_path[MAX_PATH] = {};

void write_minidump(EXCEPTION_POINTERS* info) {
    if (g_dump_path[0] == L'\0') {
        return;
    }
    HANDLE file = CreateFileW(g_dump_path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    MINIDUMP_EXCEPTION_INFORMATION exception{GetCurrentThreadId(), info, FALSE};
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                      static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory),
                      info != nullptr ? &exception : nullptr, nullptr, nullptr);
    CloseHandle(file);
    const char line[] = "[conker] minidump written (crash-reports, beside this log at the next start)\n";
    write_stderr(line, sizeof(line) - 1);
}

LONG WINAPI crash_filter(EXCEPTION_POINTERS* info) {
    claim_report();
    write_minidump(info);
    const EXCEPTION_RECORD* record = info->ExceptionRecord;
    char what[64];
    void* address = record->ExceptionAddress;
    if ((record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION || record->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
        record->NumberParameters >= 2) {
        // The data address the access was for, as si_addr gives it on POSIX.
        address = reinterpret_cast<void*>(record->ExceptionInformation[1]);
    }
    std::snprintf(what, sizeof(what), "fatal exception 0x%08lX", record->ExceptionCode);
    report_crash(what, address);
    print_backtrace(info->ContextRecord);
    TerminateProcess(GetCurrentProcess(), 128 + 11);  // the exit status a SIGSEGV gives on POSIX
    return EXCEPTION_EXECUTE_HANDLER;
}
#else
void crash_handler(int sig, siginfo_t* info, void* /*uctx*/) {
    char what[32];
    std::snprintf(what, sizeof(what), "fatal signal %d", sig);
    report_crash(what, info->si_addr);
    char buf[128];
    const int len = std::snprintf(buf, sizeof(buf), "[conker] image slide 0x%llx (tools/symbolicate.sh)\n", static_cast<unsigned long long>(_dyld_get_image_vmaddr_slide(0)));
    write_stderr(buf, len);
#if !CONKER_PS5
    void* frames[64];
    const int count = backtrace(frames, 64);
    backtrace_symbols_fd(frames, count, 2);
#endif
    _exit(128 + sig);
}
#endif

// librecomp calls std::exit on fatal runtime errors (e.g. a function-pointer call to an address
// with no function). Show the calling thread's recent recompiled calls on the way out.
void dump_trace_at_exit() {
    if (conker::current_context() != nullptr) {
        conker::trace::dump_recent_calls(2);
    }
}

// An exception nobody caught (the renderer failing to start throws one) ends the game through
// std::terminate, which no signal handler or exception filter sees: say what it was, and where.
[[noreturn]] void terminate_handler() {
#if defined(_WIN32)
    claim_report();
#endif
    char buf[640];
    int len = std::snprintf(buf, sizeof(buf), "[conker] fatal: uncaught exception\n");
#if defined(_WIN32)
    if (t_last_throw[0] != '\0') {
        len = std::snprintf(buf, sizeof(buf), "[conker] fatal: uncaught exception %s\n", t_last_throw);
    }
#endif
    if (std::exception_ptr current = std::current_exception()) {
        try {
            std::rethrow_exception(current);
        } catch (const std::exception& e) {
            len = std::snprintf(buf, sizeof(buf), "[conker] fatal: uncaught exception: %s\n", e.what());
        } catch (...) {
        }
    }
    write_stderr(buf, len);
#if defined(_WIN32)
    CONTEXT context{};
    RtlCaptureContext(&context);
    EXCEPTION_RECORD record{};
    record.ExceptionCode = 0xE0000001;  // ours: an uncaught C++ exception
    EXCEPTION_POINTERS pointers{&record, &context};
    write_minidump(&pointers);
    print_backtrace(&context);
    TerminateProcess(GetCurrentProcess(), 128 + 6);  // the exit status a SIGABRT gives on POSIX
#elif !CONKER_PS5
    void* frames[64];
    const int count = backtrace(frames, 64);
    backtrace_symbols_fd(frames, count, 2);
#endif
    _exit(128 + 6);
}

#if defined(_WIN32)
// The Microsoft C runtime keeps one terminate handler per thread, so set_terminate in main covers
// the main thread only: an exception escaping the renderer's or another library's thread would end
// in abort() with nothing said. This TLS callback runs for every thread as it starts and installs
// the handler there too.
void NTAPI terminate_on_every_thread(PVOID, DWORD reason, PVOID) {
    if (reason == DLL_THREAD_ATTACH) {
        std::set_terminate(terminate_handler);
    }
}
// The C runtime's own ways to end the process (abort(), a bad argument to a CRT function, a pure
// virtual call) skip the exception filter: report them the same way, with a minidump.
[[noreturn]] void crt_fatal(const char* what) {
    claim_report();
    char buf[160];
    const int len = std::snprintf(buf, sizeof(buf), "\n[conker] fatal: %s\n", what);
    write_stderr(buf, len);
    CONTEXT context{};
    RtlCaptureContext(&context);
    EXCEPTION_RECORD record{};
    record.ExceptionCode = 0xE0000002;  // ours: the C runtime ended the process
    EXCEPTION_POINTERS pointers{&record, &context};
    write_minidump(&pointers);
    print_backtrace(&context);
    TerminateProcess(GetCurrentProcess(), 128 + 6);
    _exit(128 + 6);
}
#endif

void install_crash_handler() {
    std::atexit(dump_trace_at_exit);
#if CONKER_PS5
    // The log in the title folder and its own crash handlers: backtrace() is a system call a title may
    // not make (ps5_platform.cpp).
    conker::ps5::init();
    return;
#endif
    std::set_terminate(terminate_handler);
#if defined(_WIN32)
    SetUnhandledExceptionFilter(crash_filter);
    AddVectoredExceptionHandler(1, note_cxx_throw);
    signal(SIGABRT, [](int) { crt_fatal("abort() called"); });
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _set_invalid_parameter_handler([](const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {
        crt_fatal("invalid parameter passed to a C runtime function");
    });
    _set_purecall_handler([] { crt_fatal("pure virtual function call"); });
#else
    struct sigaction action{};
    action.sa_sigaction = crash_handler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    sigaction(SIGSEGV, &action, nullptr);
    sigaction(SIGBUS, &action, nullptr);
    sigaction(SIGILL, &action, nullptr);
    sigaction(SIGFPE, &action, nullptr);
#endif
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

#if defined(_WIN32)
// Outside the unnamed namespace: the linker keeps it by name (/INCLUDE).
#pragma section(".CRT$XLC", read)
extern "C" __declspec(allocate(".CRT$XLC")) const PIMAGE_TLS_CALLBACK conker_terminate_tls = terminate_on_every_thread;
#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:conker_terminate_tls")
#endif

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

// Set as main returns from a normal quit (the window closed, Cmd-Q).
std::atomic<bool> g_quitting_normally{false};

int run(int argc, char** argv);

int main(int argc, char** argv) {
    const int status = run(argc, argv);
#if CONKER_LAUNCH_UI
    // Every way back here is a run that ended as it meant to; crashes, aborts and the runtime's
    // fatal-error exits never get here, so their log is kept at the next start (crash_log.hpp).
    std::printf("[conker] quit normally (status %d)\n", status);
    conker::crash_log::mark_clean_exit();
#endif
    return status;
}

int run(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // keep log order intact if the game thread crashes
    // Registered first, so it runs last of the exit handlers (the online LEAVE, the router's port, the
    // triggers let go) and before the destructors of the statics made before main. The game's own
    // threads (the scheduler, audio, the idle thread) are still running when main returns; ended here,
    // none of them meets a destroyed object (a crash on quit now and then). Other exits keep their
    // status and ordinary teardown.
    std::atexit([] {
        if (g_quitting_normally.load()) {
            std::fflush(nullptr);
            std::_Exit(0);
        }
    });
    install_crash_handler();
    parse_scripted_input();
    if (!conker::net::parse_args(&argc, argv)) {
        return 2;
    }
    // Where the game was started: a ROM named on the command line is a path from here, not from the
    // data folder a launch may enter next.
    std::error_code launch_error;
    const std::filesystem::path launch_folder = std::filesystem::current_path(launch_error);
#if CONKER_LAUNCH_UI
    conker::launch::enter_data_folder();
    if (std::filesystem::current_path(launch_error) != launch_folder) {
        conker::input_config::reload();
        conker::frame_input::reload_input_settings();
    }
    conker::crash_log::log_system_info();
    conker::launch::warn_if_emulated();
#if defined(_WIN32)
    if (!conker::crash_log::reports_folder().empty()) {
        std::error_code error;
        std::filesystem::create_directories(conker::crash_log::reports_folder(), error);
        const std::wstring dump = (conker::crash_log::reports_folder() / "pending.dmp").wstring();
        if (dump.size() < MAX_PATH) {
            std::wcsncpy(g_dump_path, dump.c_str(), MAX_PATH - 1);
        }
    }
#endif
    if (conker::launch::from_desktop() && conker::crash_log::previous_crash()) {
        // Nobody reads the log of a game started from the desktop: say where the failed run's is.
        const std::filesystem::path log = *conker::crash_log::previous_crash();
        const std::string text = "The game did not close normally last time.\n\nIf it crashed or failed to start, "
                                 "please attach its log, " + log.filename().string() +
                                 " (and a .dmp file of the same name, if there is one), when you report the "
                                 "problem. It is in the crash-reports folder.";
#if defined(__APPLE__)
        SDL_InitSubSystem(SDL_INIT_VIDEO);
#endif
        const SDL_MessageBoxButtonData buttons[] = {
            {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Continue"},
            {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Show the log"},
        };
        const SDL_MessageBoxData box{SDL_MESSAGEBOX_INFORMATION, nullptr, "Conker's Bad Fur Day Recompiled",
                                     text.c_str(), SDL_arraysize(buttons), buttons, nullptr};
        int pressed = 0;
        if (SDL_ShowMessageBox(&box, &pressed) == 0 && pressed == 1) {
            // The folder, in Finder, Explorer or the desktop's file manager.
            // file:///C:/... on Windows, file:///home/... style elsewhere, spaces and the like escaped.
            std::string url = "file://";
            const std::string folder = log.parent_path().generic_string();
            if (folder.empty() || folder[0] != '/') {
                url += '/';
            }
            for (const unsigned char c : folder) {
                if (std::isalnum(c) || std::strchr("/-_.~:", c) != nullptr) {
                    url += static_cast<char>(c);
                } else {
                    char escaped[4];
                    std::snprintf(escaped, sizeof(escaped), "%%%02X", c);
                    url += escaped;
                }
            }
            SDL_OpenURL(url.c_str());
        }
    }
#endif

    // Stored ROM copy, saves and mod folders live here.
    const std::filesystem::path data_dir = conker::data_folder();
    std::filesystem::create_directories(data_dir);
    recomp::register_config_path(data_dir);
#if CONKER_PS5
    std::printf("[conker] PS5: data folder %s\n", data_dir.c_str());
#endif
    recomp::register_game(conker::make_game_entry());
    conker::register_overlays();

    // A ROM given on the command line replaces the stored copy; without one the stored copy is used.
    std::filesystem::path rom_path = argc >= 2 ? std::filesystem::path(argv[1]) : std::filesystem::path();
    if (!rom_path.empty() && rom_path.is_relative() && !launch_folder.empty()) {
        rom_path = launch_folder / rom_path;
    }
    if (rom_path.empty()) {
        recomp::check_all_stored_roms();
    }
    if (rom_path.empty() && !recomp::is_rom_valid(conker::kGameId)) {
#if CONKER_PS5
        // No file dialog: the ROM the player put in the title folder (kept as the stored copy after this).
        rom_path = conker::ps5::find_rom();
        std::printf("[conker] ROM in the title folder: %s\n", rom_path.empty() ? "none" : rom_path.c_str());
#endif
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

#if CONKER_PS5
    std::printf("[conker] PS5: ROM ready, starting the runtime\n");
#endif
    recomp::start_game(conker::kGameId, "");
    recomp::start(cfg);
    g_quitting_normally.store(true);
    return 0;
}
