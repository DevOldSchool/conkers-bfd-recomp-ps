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
        func = aspMain;
        what = "n_aspMain (RSPRecomp)";
    }
    {
        std::lock_guard lock{seen_mutex};
        if (seen_ucodes.insert(task->t.ucode).second) {
            std::printf("[conker] RSP task type %u: ucode 0x%08X (%u bytes), data 0x%08X (%u bytes); %s\n",
                        task->t.type, task->t.ucode, task->t.ucode_size, task->t.ucode_data, task->t.ucode_data_size, what);
        }
    }
    return func;
}

} // namespace conker::rsp
