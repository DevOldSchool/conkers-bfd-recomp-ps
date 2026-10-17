#pragma once

#include <cstdint>

#include "recomp.h"
#include "librecomp/game.hpp"

namespace conker {

// XXH3-64 of the US ROM (big-endian .z64, 64 MiB), as librecomp hashes it.
constexpr uint64_t kUsRomHash = 0x23FBBA2DBCF2FD8EULL;
constexpr char8_t kGameId[] = u8"conker_us";

recomp::GameEntry make_game_entry();

// Native replacement for the ROM entry stub and TLB setup: clears bss, sets $sp, calls the first
// C function of the main executable.
void entrypoint(uint8_t* rdram, recomp_context* ctx);

// Runs after librecomp's generic init (ROM copied to RAM, IPL3 variables set) and before the
// entrypoint: installs the address-space windows, inflates the game overlay, registers functions.
void on_init(uint8_t* rdram, recomp_context* ctx);

// Puts a thread context into the FR=1 FPU register layout the game runs with (see game.cpp).
void use_fr1_float_mode(recomp_context* ctx);

// The recomp context of the N64 thread running on this host thread (diagnostics).
void set_current_context(recomp_context* ctx);
recomp_context* current_context();

// Buttons held on controller 1 right now, N64 bit order (main.cpp). Nothing is pressed unless
// CONKER_PRESS_START=<vi>[,<vi>...] asks for Start to be held for 10 VIs from each listed VI.
uint16_t scripted_buttons();

// Implemented in overlays.cpp (which includes the generated recomp_overlays.inl).
void register_overlays();
void register_runtime_addresses();

} // namespace conker
