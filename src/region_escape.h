// Escapes out of Rare's handwritten geometry code (the shared code region 0x150A3A70:0x150AE35C).
// func_150ADAF0 keeps its stack pointer in D_800D3858 on entry; func_150AE280, called from deep
// inside the work it starts (through recompiled C functions outside the region), restores that
// stack pointer and returns from func_150ADAF0 to its caller, abandoning everything in between.
// The recompiled code has real C frames in between, so the escape is a longjmp: the hooks in
// config/us.toml arm a jump buffer in the region body after func_150ADAF0 saves $sp, and
// func_150AE280 jumps back to it. Included by the generated code (recomp_include), because
// setjmp must be called in the frame that the jump returns to.
#pragma once

#include <setjmp.h>

#include "recomp.h"

#if defined(_WIN32)
#define CONKER_SETJMP(buf) setjmp(buf)
#else
#define CONKER_SETJMP(buf) _setjmp(buf)  // no signal mask: func_150ADAF0 runs many times a frame
#endif

#ifdef __cplusplus
extern "C" {
#endif
// Records the stack pointer func_150ADAF0 just saved and returns this thread's jump buffer.
jmp_buf* conker_geometry_escape_arm(recomp_context* ctx);
// func_150ADAF0's epilogue: the frame is going away, so the buffer is no longer valid.
void conker_geometry_escape_disarm(uint8_t* rdram, recomp_context* ctx);
// func_150AE280: does its work (load and clear D_800D3858, restore $sp) and jumps back to the
// armed region frame, which continues at func_150ADAF0's epilogue. Returns without jumping when
// no frame matches, leaving the original instructions to run.
void conker_geometry_escape(uint8_t* rdram, recomp_context* ctx);
#ifdef __cplusplus
}
#endif
