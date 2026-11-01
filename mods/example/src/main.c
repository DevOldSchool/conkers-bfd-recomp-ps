// Example mod: unlocks the multiplayer characters the cheat codes unlock, without typing them.
#include "modding.h"
#include "conker.h"

#define MULTIPLAYER_CHEATS (CHEAT_EATBOX | CHEAT_WELLYTOP | CHEAT_EASTEREGGSRUS | CHEAT_RUSTYSHERIFFSBADGE | \
                            CHEAT_CHINDITVICTORY | CHEAT_BEEFCURTAINS | CHEAT_BILLYMILLROUNDABOUT)

// func_151EEFF0 is the only code that clears the cheats, once, as the game starts (from
// func_15007830, the game overlay's entry point). Entering these just after it is enough.
RECOMP_HOOK_RETURN("func_151EEFF0") void unlock_multiplayer_characters(void) {
    cheat_flags |= MULTIPLAYER_CHEATS;
}
