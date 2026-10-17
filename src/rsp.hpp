#pragma once

#include "librecomp/rsp.hpp"
#include "ultramodern/ultra64.h"

namespace conker::rsp {

RspUcodeFunc* get_rsp_microcode(const OSTask* task);

} // namespace conker::rsp
