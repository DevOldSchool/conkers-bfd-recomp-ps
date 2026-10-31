// Function-entry code for the recompiled code. N64Recomp emits TRACE_ENTRY()/TRACE_RETURN()
// at the start and every return of each function when `trace_mode` is on.
//
// TRACE_ENTRY() first runs the function a mod replaced it with, if any (CONKER_MOD_REDIRECT).
// With CONKER_TRACE defined both also record the call sequence in a per-thread ring buffer that
// the crash reporter prints (src/runtime/trace.cpp); otherwise that part compiles to nothing.
#pragma once

#include "recomp.h"

#ifdef __cplusplus
extern "C" {
#endif
void conker_trace_entry(const char* name);
void conker_trace_return(const char* name);

// Bumped by librecomp whenever a mod replaces or hooks a function (its patch_func).
extern volatile uint32_t recomp_function_redirect_generation;
// The function a mod put in place of the recompiled function `name`, or null (overlays.cpp).
recomp_func_t* conker_find_redirect(const char* name);
#ifdef __cplusplus
}
#endif

// librecomp would write a jump over the function's start, which macOS on Apple silicon does not
// allow, so each function checks for a replacement itself. Until a mod patches something the
// generation stays at 0 and this costs two loads and a branch.
#define CONKER_MOD_REDIRECT() \
    static recomp_func_t* conker_redirect_; \
    static uint32_t conker_redirect_seen_; \
    if (__builtin_expect(conker_redirect_seen_ != recomp_function_redirect_generation, 0)) { \
        conker_redirect_seen_ = recomp_function_redirect_generation; \
        conker_redirect_ = conker_find_redirect(__func__); \
    } \
    if (__builtin_expect(conker_redirect_ != 0, 0)) { \
        conker_redirect_(rdram, ctx); \
        return; \
    }

#if defined(CONKER_TRACE)
#define TRACE_ENTRY() CONKER_MOD_REDIRECT() conker_trace_entry(__func__);
#define TRACE_RETURN() conker_trace_return(__func__);
#else
#define TRACE_ENTRY() CONKER_MOD_REDIRECT()
#define TRACE_RETURN()
#endif
