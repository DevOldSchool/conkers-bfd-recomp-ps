// Hashes of the game's memory once a game frame, for finding where two runs of the same input part
// ways. CONKER_STATE_HASH_LOG=<file> writes a line a frame:
// the frame number, then one 32-bit hash per 16 KiB block of RDRAM from 0x80000000 to 0x80400000
// (256 of them), taken at the start of func_1501905C, before the frame's input record. Compare two
// logs with tools/compare_state_hashes.py. CONKER_STATE_HASH_EVERY=<n> hashes every nth frame only.
// CONKER_STATE_DUMP_FRAMES=<n>[,<n>...] and CONKER_STATE_DUMP_DIR=<dir> write the whole 4 MiB at
// those frames, for finding which bytes differ, and CONKER_STATE_WATCH=<start>-<end>[,...] with
// CONKER_STATE_WATCH_LOG=<file> chosen ranges every frame.
#pragma once

#include <cstdint>

namespace conker::state_hash {

// Called once a game frame from the frame input hook with the frame about to start.
void frame(uint8_t* rdram, uint32_t frame);

// The game frame running now (0 before the first).
uint32_t current_frame();

}  // namespace conker::state_hash
