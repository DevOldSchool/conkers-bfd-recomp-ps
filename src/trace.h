// Function-entry trace for the recompiled code. N64Recomp emits TRACE_ENTRY()/TRACE_RETURN()
// at the start and every return of each function when `trace_mode` is on. With CONKER_TRACE
// defined they record the call sequence in a per-thread ring buffer that the crash reporter
// prints (src/trace.cpp); otherwise they compile to nothing.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif
void conker_trace_entry(const char* name);
void conker_trace_return(const char* name);
#ifdef __cplusplus
}
#endif

#if defined(CONKER_TRACE)
#define TRACE_ENTRY() conker_trace_entry(__func__);
#define TRACE_RETURN() conker_trace_return(__func__);
#else
#define TRACE_ENTRY()
#define TRACE_RETURN()
#endif
