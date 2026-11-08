// How each weapon and event feels: its haptic effects, adaptive-trigger effect, rumble character and
// control numbers, from conker-recomp-data/feel.cfg, which the game writes with the built-in values on
// first start and reads as it starts, on every platform (again within half a second of a change with
// CONKER_FEEL_LIVE=1). Copying the file
// to another machine gives it the same feel. See docs/enhancements.md.
//
// Online play keeps the built-in control numbers: they act on the game in the pad hook, on every
// machine alike, and two machines must stay the same to the bit. Haptics, triggers and rumble only
// touch this machine's pad, so they follow the file online too.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "platform/input.hpp"

namespace conker::feel {

// A haptic repeated on a beat while a weapon is in hand, alternating grips if asked.
struct Pulse {
    conker::input::Haptic haptic;
    float every = 0.1f;  // seconds
    bool alternate = false;
};

// Whether CONKER_FEEL_LIVE=1 is set: the file read again as it is saved, and the weapon survey. Without it the file is read once, as the game starts.
bool live();

// Live, reads the file again if it changed since the last look (at most every half second). Any thread.
void refresh();

// Bumps each time the file's values are taken in, for users that keep values of their own.
uint32_t generation();

// An event's haptic effects ("landing", "gun_round"...): the parts play together. Empty if none.
std::vector<conker::input::Haptic> haptic(const char* name);

// A trigger effect ("mounted_gun", "uzis"...). Off if there is none.
conker::input::TriggerEffect trigger(const char* name);

// A rumble character ("uzis"...). The deep default if there is none.
conker::input::RumbleFeel rumble(const char* name);

// A weapon's name as feel.cfg calls it: "item_23" stays itself unless the file names it
// ("item_23.name = magnum"), and then all of its lines use that name ("magnum.trigger").
std::string name(const std::string& id);

// The weapon an item is (0x23 "revolver"; one not known "item_<hex>"), as the file names it.
std::string weapon_name(uint8_t item);
// The same with the built-in name always, for its controls.<weapon> lines: online they are the
// built-in values, which a name the file gave would miss.
std::string weapon_key(uint8_t item);

// A weapon's own rumble while it is in hand (its .hold line); false for none.
bool hold(const std::string& weapon, conker::input::RumbleFeel* feel);

// A weapon's own rumble while its use button is held (its .use line) and that button ('a', 'b', 'z'
// or 'r', its .use_button line, 'z' (the fire button in a match) if none); false for none.
bool use(const std::string& weapon, conker::input::RumbleFeel* feel, char* button);

// The N64 buttons that count as using a weapon (its .use_button line; Z, the fire button in a match, if
// none).
uint16_t use_buttons(const std::string& weapon);

// A weapon's shot haptic (its .shot line, "every" unused); false for none.
bool shot(const std::string& weapon, Pulse* out);

// A weapon's pulse: its .use_pulse while used (if it has one), else its .pulse; false for none.
bool pulse(const std::string& weapon, bool used, Pulse* out);

// The haptic a weapon plays as one of its sounds starts (its .sounds and .sound lines); empty if
// that sound is not one of them.
std::vector<conker::input::Haptic> sound_haptic(const std::string& weapon, int sound);

// A number ("controls.match_turn", "gun_round.alternate", "pee.from"...). 0 if there is none; online, a
// "controls." one is always the built-in value.
float number(const char* name);

}  // namespace conker::feel
