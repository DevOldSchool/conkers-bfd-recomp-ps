// The PS5 title's start-up (src/platform/ps5_platform.hpp).
#include "platform/ps5_platform.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>

#include <SDL.h>
#include <ps5platform/kernel.h>

// libSceSystemService, which the SDK's headers do not declare.
extern "C" int sceSystemServiceHideSplashScreen(void);

// Every import of the title by name and by its address once loaded, written by the title link
// (tools/ps5/link-title.sh): an import the console lacks is left at 0, and a call to one ends the
// game. Weak, so a title linked without the table still starts.
extern "C" {
extern const void* const conker_ps5_import_addrs[] __attribute__((weak));
extern const char* const conker_ps5_import_names[] __attribute__((weak));
extern const unsigned int conker_ps5_import_count __attribute__((weak));
}

namespace conker::ps5 {

namespace {

constexpr const char* kLogPath = "/app0/conker.log";

int g_log_fd = 2;

// The crash report's output: write() alone, which a signal handler may call.
void write_text(const char* text) {
    size_t length = std::strlen(text);
    while (length > 0) {
        const ssize_t written = write(g_log_fd, text, length);
        if (written <= 0) {
            return;
        }
        text += written;
        length -= static_cast<size_t>(written);
    }
}

void write_hex(uint64_t value) {
    char text[19] = "0x";
    for (int i = 0; i < 16; i++) {
        text[2 + i] = "0123456789abcdef"[(value >> (60 - 4 * i)) & 0xF];
    }
    text[18] = '\0';
    write_text(text);
}

void write_decimal(uint64_t value) {
    char text[21];
    int at = 20;
    text[at] = '\0';
    do {
        text[--at] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0 && at > 0);
    write_text(text + at);
}

const char* signal_name(int sig) {
    switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS: return "SIGBUS";
    case SIGILL: return "SIGILL";
    case SIGFPE: return "SIGFPE";
    case SIGABRT: return "SIGABRT";
    default: return "signal";
    }
}

// The console starts a title with no environment of its own, so the switches a desktop build takes as
// environment variables (CONKER_..., docs/diagnostics.md) come from conker-env.txt in the title folder:
// a NAME=value a line, # for a comment. Read before the C++ static initialisers, so every reader sees them.
constexpr const char* kEnvPath = "/app0/conker-env.txt";

void load_env() {
    FILE* file = std::fopen(kEnvPath, "r");
    if (file == nullptr) {
        return;
    }
    char line[1024];
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        size_t length = std::strcspn(line, "\r\n");
        line[length] = '\0';
        char* name = line;
        while (*name == ' ' || *name == '\t') {
            name++;
        }
        char* equals = std::strchr(name, '=');
        if (*name == '\0' || *name == '#' || equals == nullptr || equals == name) {
            continue;
        }
        *equals = '\0';
        for (char* end = equals; end > name && (end[-1] == ' ' || end[-1] == '\t'); end--) {
            end[-1] = '\0';
        }
        setenv(name, equals + 1, 1);
        std::printf("[conker] PS5: conker-env.txt sets %s=%s\n", name, equals + 1);
    }
    std::fclose(file);
}

// The log, new each run, before main: stdout and stderr go to it unbuffered, so a crash loses
// nothing, and a crash before main still leaves a line saying the title started. A constructor of
// the first priority, ahead of the C++ static initialisers.
// dup2 onto descriptors 1 and 2 leaves the file empty on the console, so the log keeps a descriptor
// of its own (for the crash report's async-signal-safe writes) and stdout and stderr are reopened on
// the same file. Everything appends, so the three never write over each other.
__attribute__((constructor(101))) void open_log() {
    const int fd = open(kLogPath, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0666);
    if (fd < 0) {
        return;
    }
    g_log_fd = fd;
    std::freopen(kLogPath, "a", stdout);
    std::freopen(kLogPath, "a", stderr);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    write_text("[conker] PS5: Conker's Bad Fur Day Recompiled is starting\n");
    load_env();
}

// A crash report: the signal, the address it was about, the registers and the return addresses
// found by walking the frame pointers, with async-signal-safe calls only. backtrace() is a call a
// title may not make. The first thread to crash reports; any other waits for the process to end.
std::atomic<bool> g_crashed{false};

void crash_handler(int sig, siginfo_t* info, void* context) {
    if (g_crashed.exchange(true)) {
        for (;;) {
            pause();
        }
    }
    write_text("\n[conker] fatal ");
    write_text(signal_name(sig));
    write_text(" (");
    write_decimal(static_cast<uint64_t>(sig));
    write_text(") at address ");
    write_hex(reinterpret_cast<uintptr_t>(info != nullptr ? info->si_addr : nullptr));
    write_text("\n");

    const ucontext_t* uc = static_cast<const ucontext_t*>(context);
    const uint64_t rip = static_cast<uint64_t>(uc->uc_mcontext.mc_rip);
    const uint64_t rsp = static_cast<uint64_t>(uc->uc_mcontext.mc_rsp);
    const uint64_t rbp = static_cast<uint64_t>(uc->uc_mcontext.mc_rbp);
    write_text("[conker] rip ");
    write_hex(rip);
    write_text(" rsp ");
    write_hex(rsp);
    write_text(" rbp ");
    write_hex(rbp);
    // Where the title was loaded: this function's address here, against its address in the symbol
    // table of eboot.bin.elf (the link's unsigned copy), gives the offset for every address below.
    write_text("\n[conker] crash_handler is at ");
    write_hex(reinterpret_cast<uintptr_t>(&crash_handler));
    write_text("\n[conker] backtrace (frame pointers):\n  #0 ");
    write_hex(rip);
    write_text("\n");

    // Each frame holds the caller's frame pointer and then the return address. Only frames above the
    // stack pointer and within 16 MiB of it are followed, so a frame pointer the code reused for
    // something else ends the walk instead of faulting again.
    uint64_t frame = rbp;
    for (int depth = 1; depth < 64; depth++) {
        if (frame < rsp || frame - rsp > (16u << 20) || (frame & 7) != 0) {
            break;
        }
        const uint64_t* words = reinterpret_cast<const uint64_t*>(frame);
        const uint64_t next = words[0];
        const uint64_t return_address = words[1];
        if (return_address == 0) {
            break;
        }
        write_text("  #");
        write_decimal(static_cast<uint64_t>(depth));
        write_text(" ");
        write_hex(return_address);
        write_text("\n");
        if (next <= frame) {
            break;
        }
        frame = next;
    }
    write_text("[conker] end of crash report\n");
    _exit(128 + sig);
}

void install_crash_handlers() {
    // The main thread's handler runs on a stack of its own, so a stack overflow there is reported too.
    static std::vector<uint8_t> alternate_stack(256 * 1024);
    stack_t stack{};
    stack.ss_sp = alternate_stack.data();
    stack.ss_size = alternate_stack.size();
    sigaltstack(&stack, nullptr);

    struct sigaction action{};
    action.sa_sigaction = crash_handler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) {
        sigaction(sig, &action, nullptr);
    }
}

void report_memory() {
    size_t flexible = 0;
    const int32_t result = sceKernelAvailableFlexibleMemorySize(&flexible);
    if (result == 0) {
        std::printf("[conker] PS5: %zu MiB of flexible memory available\n", flexible >> 20);
    } else {
        std::printf("[conker] PS5: sceKernelAvailableFlexibleMemorySize failed (0x%08X)\n", static_cast<unsigned>(result));
    }
    std::printf("[conker] PS5: %lld MiB of direct memory\n",
                static_cast<long long>(sceKernelGetDirectMemorySize() >> 20));
}

// Imports the console does not have are found here, by name, rather than as a crash at their first call.
void report_missing_imports() {
    if (&conker_ps5_import_count == nullptr || conker_ps5_import_addrs == nullptr || conker_ps5_import_names == nullptr) {
        std::printf("[conker] PS5: this build has no import table\n");
        return;
    }
    unsigned int missing = 0;
    for (unsigned int i = 0; i < conker_ps5_import_count; i++) {
        if (conker_ps5_import_addrs[i] == nullptr) {
            std::printf("[conker] PS5: missing import %s\n", conker_ps5_import_names[i]);
            missing++;
        }
    }
    std::printf("[conker] PS5: %u imports, %u missing\n", conker_ps5_import_count, missing);
}

}  // namespace

void init() {
    install_crash_handlers();
    report_memory();
    report_missing_imports();
    // SDL has no display driver for the console; Vulkan presents to the display without it
    // (tools/patches/plume-ps5.patch), so its offscreen driver is all the game's window needs.
    SDL_SetHint(SDL_HINT_VIDEODRIVER, "offscreen");
    const int splash = sceSystemServiceHideSplashScreen();
    if (splash != 0) {
        std::printf("[conker] PS5: sceSystemServiceHideSplashScreen failed (0x%08X)\n", static_cast<unsigned>(splash));
    }
}

void flush_log() {
    std::fflush(stdout);
    std::fflush(stderr);
    fsync(g_log_fd);
}

std::filesystem::path find_rom() {
    std::vector<std::filesystem::path> roms;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(kTitleFolder, error)) {
        const std::string extension = entry.path().extension().string();
        if ((extension == ".z64" || extension == ".n64" || extension == ".v64") && entry.is_regular_file(error)) {
            roms.push_back(entry.path());
        }
    }
    if (roms.empty()) {
        return {};
    }
    // The directory's order is the file system's: by name, so the choice is the same every time.
    return *std::min_element(roms.begin(), roms.end());
}

}  // namespace conker::ps5
