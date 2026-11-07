// A snapshot of the running game, taken at the start of a game frame (the input latch in
// func_1501905C) and put in place of another run's at the same point, so that a machine rejoining
// an online session starts from the host's game instead of replaying it from frame 1.
//
// At that point the main game thread is the one running, and every other game thread is blocked in
// osRecvMesg one level down its own loop function (the scheduler func_800049E0, audio func_80009400,
// func_80002E50, func_80003658, func_100052A0; the idle thread spins in func_800010F8), the same on a
// machine that has just started as in the middle of a match. A thread's place is its native stack,
// which a snapshot cannot carry, so a snapshot only goes where every thread waits at the same call
// (return address and queue) as where it was taken; then the game's memory (the first 4 MiB of
// RDRAM), each thread's registers, the timers set, the save and the native state the game's memory
// depends on (registered with add) make the two runs the same.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "recomp.h"

namespace conker::snapshot {

// The most a snapshot can be (its 4 MiB of game memory, the threads, timers, save and native state):
// a guest refuses a bigger one before allocating it.
constexpr uint32_t kMaxSize = 8u * 1024 * 1024;

// Native state a module keeps that the game's memory depends on, copied byte for byte.
void add(const char* name, void* data, size_t size);
// The same, once `valid` accepts the snapshot's bytes: a snapshot comes from another machine, so a field
// that feeds an address, a count or a loop is checked before anything is put in place.
void add(const char* name, void* data, size_t size, std::function<bool(const uint8_t*)> valid);
// An array of bools: each byte 0 or 1.
void add_bools(const char* name, bool* data, size_t count);
// Or saved and loaded by the module (load returns false if it cannot).
void add(const char* name, std::function<std::vector<uint8_t>()> save, std::function<bool(const std::vector<uint8_t>&)> load);

// A condition a snapshot needs, the same where it is taken and where it is put: the reason it does
// not hold (a task still out, say), or nullptr.
void add_check(std::function<const char*()> check);

// From each game thread as it starts, with its registers (game.cpp).
void note_thread(recomp_context* ctx);

// The registers of the game thread waiting on the message queue at `queue` (KSEG0), or nullptr when
// none is (game thread, at the start of a frame).
// Each known thread's OSThread, state and the queue it waits on, for logs.
std::string describe_threads(uint8_t* rdram);
recomp_context* thread_waiting_on(uint8_t* rdram, uint32_t queue);

// On the game thread at the start of a frame (see frame_input.cpp): the game as it is now, or empty
// with the reason when it cannot be taken here (a task still out, say).
std::vector<uint8_t> save(uint8_t* rdram, recomp_context* ctx, uint32_t frame, std::string* why);

// The frame a snapshot was taken at (0 if it is not one).
uint32_t frame_of(const std::vector<uint8_t>& snapshot);

// On the game thread at the start of a frame: puts the snapshot in place. False, with the reason and
// nothing changed, when this run is not at the same point.
bool load(uint8_t* rdram, recomp_context* ctx, const std::vector<uint8_t>& snapshot, std::string* why);

// CONKER_SNAPSHOT_SELFTEST=<frame> (docs/diagnostics.md): takes a snapshot at that frame, then loads copies
// of it whose runtime structures were changed as a hostile host's could be (a queue's indices or buffer
// out of range, a waiting thread that is not one of this run's, a list that loops) and checks that each
// is refused with the game's memory untouched. Returns a line per case, each "PASS ..." or "FAIL ...".
std::vector<std::string> self_test(uint8_t* rdram, recomp_context* ctx, uint32_t frame);

// RT64 draws a frame on top of what it drew before (its frame buffers' undrawn pixels come from its
// own history, not from RDRAM), so a run whose memory was replaced would write its old pictures back
// over the snapshot's. So the run that loads a snapshot, the one that takes it and every other peer
// of an online session make the renderer forget its frame buffers before that frame's display list,
// after which all of them draw from the same RDRAM alike. Taking or loading a snapshot asks for it;
// request_framebuffer_discard asks for it on a peer that does neither.
void request_framebuffer_discard();
// For the renderer, before the next display list: true once after a request.
bool take_framebuffer_discard();

}  // namespace conker::snapshot
