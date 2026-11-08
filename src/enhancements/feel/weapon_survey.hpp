// The weapon survey: everything one play-through can tell about each item a player holds, for naming
// feel.cfg's item_<hex> weapons and setting their controls, written to
// conker-recomp-data/weapon_survey.txt (every 2 s while something new turns up), on every platform,
// only with CONKER_FEEL_LIVE=1 (conker::feel::live()).
// Per item: where it was held (scenes, story or match), its sight and look states (u8 +0x197 and
// +0x198 of the player block), whether the player moved while aiming, the animations played and the
// sounds started while it was held, how often the game's own rumble came on, and the B pad action.
#pragma once

#include <cstdint>

namespace conker::survey {

// Once a frame, on the game thread.
void frame(uint8_t* rdram);

// A sound starting (sndp_play_sound), on the game or the audio thread.
void sound(uint8_t* rdram, uint16_t sound);

}  // namespace conker::survey
