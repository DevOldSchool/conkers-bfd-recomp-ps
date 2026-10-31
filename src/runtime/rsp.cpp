// RSP task handling.
//
// Graphics tasks never reach this file: ultramodern hands M_GFXTASK display lists to the
// renderer. Everything else arrives here. Conker's audio tasks run Rare's n_aspMain microcode with
// its MP3 overlay, recompiled by RSPRecomp into build/us/rsp/aspMain.cpp (config/aspMain.toml).
// CONKER_SILENT_AUDIO=1 acknowledges audio tasks without running the microcode, as the runtime
// did before the microcode was recompiled.
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>
#include <stdexcept>

#include "recomp.h"
#include "librecomp/rsp.hpp"
#include "ultramodern/ultra64.h"

#include "runtime/rsp.hpp"

RspExitReason aspMain(uint8_t* rdram, uint32_t ucode_addr);

namespace conker::rsp {

namespace {

RspExitReason silent_task(uint8_t* /*rdram*/, uint32_t /*ucode_addr*/) {
    return RspExitReason::Broke;
}

// RSPRecomp's overlay switch looks the overlay's addresses up with std::map::at. They come from the
// task and DMEM, so from game memory; an unknown one throws, which on the SP task thread would end
// the process with std::terminate. As an exit reason instead, the runtime stops the game cleanly.
RspExitReason checked_aspMain(uint8_t* rdram, uint32_t ucode_addr) {
    try {
        return aspMain(rdram, ucode_addr);
    } catch (const std::out_of_range&) {
        std::fprintf(stderr, "[conker] The audio task asked for a microcode overlay that does not exist: the game's memory "
                             "is not one this run can trust.\n");
        return RspExitReason::UnhandledJumpTarget;
    }
}

bool silent_audio() {
    static const bool silent = [] {
        const char* value = std::getenv("CONKER_SILENT_AUDIO");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return silent;
}

std::mutex seen_mutex;
std::set<uint32_t> seen_ucodes;

} // namespace

RspUcodeFunc* get_rsp_microcode(const OSTask* task) {
    RspUcodeFunc* func = silent_task;
    const char* what = "running silently";
    if (task->t.type == M_AUDTASK && !silent_audio()) {
        func = checked_aspMain;
        what = "n_aspMain (RSPRecomp)";
    }
    {
        std::lock_guard lock{seen_mutex};
        // Only the first few: the address is game data, and a new one each task would grow this forever.
        if (seen_ucodes.size() < 16 && seen_ucodes.insert(task->t.ucode).second) {
            std::printf("[conker] RSP task type %u: ucode 0x%08X (%u bytes), data 0x%08X (%u bytes); %s\n",
                        task->t.type, task->t.ucode, task->t.ucode_size, task->t.ucode_data, task->t.ucode_data_size, what);
        }
    }
    return func;
}

} // namespace conker::rsp
