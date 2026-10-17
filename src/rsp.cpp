// RSP task handling.
//
// Graphics tasks never reach this file: ultramodern hands M_GFXTASK display lists to the
// renderer. Everything else (Conker uses Rare's n_aspMain audio microcode with an MP3 overlay)
// arrives here. Until the audio microcode is recompiled with RSPRecomp, tasks are acknowledged
// without producing samples so that the game can run silently.
#include <cstdio>
#include <mutex>
#include <set>

#include "recomp.h"
#include "librecomp/rsp.hpp"
#include "ultramodern/ultra64.h"

#include "rsp.hpp"

namespace conker::rsp {

namespace {

RspExitReason silent_task(uint8_t* /*rdram*/, uint32_t /*ucode_addr*/) {
    return RspExitReason::Broke;
}

std::mutex seen_mutex;
std::set<uint32_t> seen_ucodes;

} // namespace

RspUcodeFunc* get_rsp_microcode(const OSTask* task) {
    {
        std::lock_guard lock{seen_mutex};
        if (seen_ucodes.insert(task->t.ucode).second) {
            std::printf("[conker] RSP task type %u: ucode 0x%08X (%u bytes), data 0x%08X (%u bytes); running silently\n",
                        task->t.type, task->t.ucode, task->t.ucode_size, task->t.ucode_data, task->t.ucode_data_size);
        }
    }
    return silent_task;
}

} // namespace conker::rsp
