// Every multiplayer character unlocked: the seven cheats that unlock them (EATBOX, WELLYTOP and the
// rest) are entered for the player. Called once a game frame from src/runtime/frame_input.cpp.
// CONKER_MP_CHARACTERS=0 leaves them to be typed on the CHEATS page.
#include "enhancements/mp_characters/mp_characters.hpp"

#include <atomic>
#include <cstdlib>

#include "recomp.h"
#include "enhancements/status.hpp"
#include "enhancements/switches.hpp"

namespace {

inline gpr kseg0(uint32_t address) { return static_cast<gpr>(static_cast<int32_t>(address)); }

// D_800E9D00: the cheats entered, a bit each (mods/include/conker.h names them). func_151EEFF0 clears
// it once as the game starts; the CHEATS page (func_151DE8F0) sets a bit for each code typed.
constexpr uint32_t kCheatFlags = 0x800E9D00;
constexpr uint32_t kMultiplayerCheats = (1u << 4)     // EASTEREGGSRUS: Neo Conker
                                        | (1u << 5)   // WELLYTOP: Conker
                                        | (1u << 6)   // CHINDITVICTORY: the Weasel henchmen
                                        | (1u << 7)   // EATBOX: the Caveman
                                        | (1u << 8)   // BEEFCURTAINS: the Zombies and Villagers
                                        | (1u << 16)  // RUSTYSHERIFFSBADGE: the Sergeant and the Tediz Leader
                                        | (1u << 17); // BILLYMILLROUNDABOUT: Gregg

std::atomic<bool> g_enabled{[] {
    const char* value = std::getenv("CONKER_MP_CHARACTERS");
    return value == nullptr || value[0] != '0';
}()};  // the debug menu switches it

// The cheats this enhancement entered, not the player: switched off, only these go again.
uint32_t g_entered = 0;

bool mp_characters_enabled() { return g_enabled.load(std::memory_order_relaxed); }

}  // namespace

// Once a game frame, so the switch acts at once, and online after the host's settings are applied
// (the first frame), as on every machine of the session alike.
void conker::mp_characters::frame(uint8_t* rdram) {
    uint32_t cheats = MEM_W(0, kseg0(kCheatFlags));
    if (mp_characters_enabled()) {
        g_entered |= kMultiplayerCheats & ~cheats;
        cheats |= kMultiplayerCheats;
    } else {
        cheats &= ~g_entered;
        g_entered = 0;
    }
    MEM_W(0, kseg0(kCheatFlags)) = cheats;
}

nlohmann::json conker::status::mp_characters() {
    return enhancement("mp_characters", "Every multiplayer character", mp_characters_enabled(),
                       "CONKER_MP_CHARACTERS");
}

conker::switches::Switch conker::switches::mp_characters() {
    return {"MP CHARACTERS", mp_characters_enabled, [](bool on) { g_enabled.store(on); }};
}
