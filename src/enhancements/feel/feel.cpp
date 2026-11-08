// conker-recomp-data/feel.cfg; see feel.hpp.
#include "enhancements/feel/feel.hpp"

#include <sys/stat.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>

#include "net/net_play.hpp"
#include "platform/data_folder.hpp"

namespace conker::feel {

namespace {

using conker::input::Haptic;
using conker::input::RumbleFeel;
using conker::input::TriggerEffect;

// The built-in values, and the file as the game first writes it. Every name the game asks for is
// here; a line the file leaves out keeps its value from here.
constexpr const char* kDefaults = R"(# How each weapon and event feels, on every platform. Read as the game starts; with CONKER_FEEL_LIVE=1
# it is read again within half a second of being saved. Delete a line to go back to its built-in
# value; delete the file to get them all back.
#
# Haptics: <event> = intensity sharpness seconds grip [; another part ...]
#   intensity and sharpness 0 to 1 (sharpness 0 a deep thud, 1 a crisp click), seconds 0 for a tap,
#   grip left, right, both or trigger (the side of the trigger last pulled).
# Triggers: <weapon>.trigger = off | feedback <from> <strength> | weapon <start> <end> <strength>
#   | vibration <start> <amplitude> <frequency>   (positions and strengths 0 to 1 along the pull)
# Rumble: <weapon>.rumble = intensity sharpness   (the game's own rumble while that weapon is held)
# Controls: controls.<name> = number   (online play always uses the built-in ones)

# --- haptics ---
landing = 0.8 0.2 0 both
splat = 1 0.1 0 both; 0.7 0.05 0.4 both
fall_death = 1 0 0 both; 1 0 0.6 both
shotgun = 1 0.5 0 both; 1 0.1 0.15 both
gun_round = 1 0 0 both; 1 0 0.22 both
# 1: each round in one grip, left then right (its parts' "both" become that grip); 0: both grips
gun_round.alternate = 1
slingshot_out = 0.5 0.6 0 right
slingshot_shot = 1 0.9 0 trigger
slingshot_elastic = 0.35 0.2 0.25 trigger
slingshot_reload = 0.4 1 0 trigger

# --- triggers and rumble ---
# Seated at a mounted machine gun (its rounds are gun_round above).
mounted_gun.trigger = feedback 0.2 0.4
# --- weapons: while one is in hand ---
#   <weapon>.trigger   its trigger effect
#   <weapon>.rumble    how the game's own rumble feels with it
#   <weapon>.hold      a rumble of its own while it is in hand (intensity sharpness; off for none)
#   <weapon>.use       a rumble of its own while its use button is held (cutting with the chainsaw)
#   <weapon>.use_button  that button, as the game sees it after your mapping: a, b, z or r, or several
#                      ("z b": either); the log names the buttons pressed with a weapon in hand
#   <weapon>.pulse     a haptic repeated while it is in hand: <haptic> every <seconds> [alternate]
#                      (alternate: left grip, then right, then left...; off for none)
#   <weapon>.use_pulse the same while its use button is held, in place of .pulse
#   <weapon>.use_linger  seconds the use goes on after the button is let go (a cut running on)
#   <weapon>.use_rumble_window  0, or: only in use while the game's own rumble came on within these
#                      seconds (a gun firing, not reloading with the trigger still held)
#   <weapon>.shot      a haptic each time the game's own rumble starts with it in hand (a shot, for
#                      most guns): <haptic> [alternate] (alternate: left grip, then right...); off for
#                      none. Set its .rumble to 0 0 to feel only these.
#   <weapon>.sounds    its sound effects that rumble while it is in hand (ids and ranges, or none;
#                      CONKER_SOUND_LOG=1 on a computer lists every sound as it plays)
#   <weapon>.sound     the haptic each of them plays (as the haptics above)
#   controls.<weapon>.stick_c   1: the left stick's up and down press C-up and C-down while aiming,
#                      zooming through a sight that zooms with them, walking through one that walks
# The log names the item in hand as it changes. Any other item works too as item_<hex>; name one once
# you know it ("item_3a.name = mallet") and its lines become mallet.trigger and so on (controls lines
# keep the built-in name: controls.item_3a.stick_c). With CONKER_FEEL_LIVE=1 the survey
# (conker-recomp-data/weapon_survey.txt) records what each item held does.
# 0x02: the slingshot, out with B (story)
slingshot.trigger = off
slingshot.rumble = 1 0
slingshot.hold = off
slingshot.use = off
slingshot.use_button = z
slingshot.pulse = off
slingshot.use_pulse = off
slingshot.use_linger = 0
slingshot.use_rumble_window = 0
slingshot.shot = off
slingshot.sounds = none
slingshot.sound = 0.8 0.3 0.15 both
controls.slingshot.stick_c = 0
# 0x09: double Uzis: R aims (no sight), walks while aiming; the Heist's too
uzis.trigger = feedback 0.2 0.25
uzis.rumble = 0 0
uzis.hold = off
uzis.use = off
uzis.use_button = z
uzis.pulse = off
uzis.use_pulse = 1 0.1 0.08 both every 0.09 alternate
uzis.use_linger = 0
uzis.use_rumble_window = 0.3
uzis.shot = off
uzis.sounds = none
uzis.sound = 0.8 0.3 0.15 both
controls.uzis.stick_c = 0
# 0x0C: It's War's rocket launcher, from its B pads (story)
story_launcher.trigger = off
story_launcher.rumble = 1 0
story_launcher.hold = off
story_launcher.use = off
story_launcher.use_button = z
story_launcher.pulse = off
story_launcher.use_pulse = off
story_launcher.use_linger = 0
story_launcher.use_rumble_window = 0
story_launcher.shot = off
story_launcher.sounds = none
story_launcher.sound = 0.8 0.3 0.15 both
controls.story_launcher.stick_c = 1
# 0x14: throwing knives: a sight, no zoom, strafes while aiming
knives.trigger = off
knives.rumble = 1 0
knives.hold = off
knives.use = off
knives.use_button = z
knives.pulse = off
knives.use_pulse = off
knives.use_linger = 0
knives.use_rumble_window = 0
knives.shot = off
knives.sounds = none
knives.sound = 0.8 0.3 0.15 both
controls.knives.stick_c = 0
# 0x15: flamethrower: R aims, walks while aiming (told from the auto rifle by its sounds)
flamethrower.trigger = off
flamethrower.rumble = 0 0
flamethrower.hold = off
flamethrower.use = off
flamethrower.use_button = z
flamethrower.pulse = off
flamethrower.use_pulse = 0.55 0.05 0.1 both every 0.07 alternate
flamethrower.use_linger = 0.2
flamethrower.use_rumble_window = 0
flamethrower.shot = off
flamethrower.sounds = none
flamethrower.sound = 0.8 0.3 0.15 both
controls.flamethrower.stick_c = 0
# 0x16: rocket launcher: a sight, walks while aiming, no zoom
rocket_launcher.trigger = off
rocket_launcher.rumble = 1 0
rocket_launcher.hold = off
rocket_launcher.use = off
rocket_launcher.use_button = z
rocket_launcher.pulse = off
rocket_launcher.use_pulse = off
rocket_launcher.use_linger = 0
rocket_launcher.use_rumble_window = 0
rocket_launcher.shot = off
rocket_launcher.sounds = none
rocket_launcher.sound = 0.8 0.3 0.15 both
controls.rocket_launcher.stick_c = 1
# 0x19: the samurai sword or the baseball bat: no aim
sword_or_bat.trigger = off
sword_or_bat.rumble = 1 0
sword_or_bat.hold = off
sword_or_bat.use = off
sword_or_bat.use_button = z
sword_or_bat.pulse = off
sword_or_bat.use_pulse = off
sword_or_bat.use_linger = 0
sword_or_bat.use_rumble_window = 0
sword_or_bat.shot = off
sword_or_bat.sounds = none
sword_or_bat.sound = 0.8 0.3 0.15 both
controls.sword_or_bat.stick_c = 0
# 0x1B: a mounted machine gun as an item
mounted_gun_item.trigger = off
mounted_gun_item.rumble = 1 0
mounted_gun_item.hold = off
mounted_gun_item.use = 1 0
mounted_gun_item.use_button = z
mounted_gun_item.pulse = off
mounted_gun_item.use_pulse = off
mounted_gun_item.use_linger = 0
mounted_gun_item.use_rumble_window = 0
mounted_gun_item.shot = off
mounted_gun_item.sounds = none
mounted_gun_item.sound = 0.8 0.3 0.15 both
controls.mounted_gun_item.stick_c = 1
# 1: a match's mounted gun aims with the right stick and zooms with the left; 0: as the story's,
# the left stick aiming and the right stick zooming
controls.mounted_gun_item.twin_stick = 0
# 0x21: the baseball bat or the samurai sword: no aim
bat_or_sword.trigger = off
bat_or_sword.rumble = 1 0
bat_or_sword.hold = off
bat_or_sword.use = off
bat_or_sword.use_button = z
bat_or_sword.pulse = off
bat_or_sword.use_pulse = off
bat_or_sword.use_linger = 0
bat_or_sword.use_rumble_window = 0
bat_or_sword.shot = off
bat_or_sword.sounds = none
bat_or_sword.sound = 0.8 0.3 0.15 both
controls.bat_or_sword.stick_c = 0
# 0x22: chainsaw: no aim; the game never rumbles for it: a dull pulse, side to side, faster cutting
# (its own sounds are 1092-1098 1416-1419 1547, for .sounds)
chainsaw.trigger = off
chainsaw.rumble = 1 0
chainsaw.hold = off
chainsaw.use = off
chainsaw.use_button = z b
chainsaw.pulse = 0.22 0.05 0.1 both every 0.12 alternate
chainsaw.use_pulse = 1 0 0.12 both every 0.1
chainsaw.use_linger = 0.5
chainsaw.use_rumble_window = 0
chainsaw.shot = off
chainsaw.sounds = none
chainsaw.sound = 0.9 0.15 0.3 both
controls.chainsaw.stick_c = 0
# 0x23: revolver: a sight with zoom, strafes but cannot walk while aiming
revolver.trigger = off
revolver.rumble = 1 0
revolver.hold = off
revolver.use = off
revolver.use_button = z
revolver.pulse = off
revolver.use_pulse = off
revolver.use_linger = 0
revolver.use_rumble_window = 0
revolver.shot = off
revolver.sounds = none
revolver.sound = 0.8 0.3 0.15 both
controls.revolver.stick_c = 1
# 0x24: sniper rifle: a sight with zoom
sniper.trigger = off
sniper.rumble = 1 0
sniper.hold = off
sniper.use = off
sniper.use_button = z
sniper.pulse = off
sniper.use_pulse = off
sniper.use_linger = 0
sniper.use_rumble_window = 0
sniper.shot = off
sniper.sounds = none
sniper.sound = 0.8 0.3 0.15 both
controls.sniper.stick_c = 1
# 0x37: a match's shotgun (TEMPLE)
match_shotgun.trigger = off
match_shotgun.rumble = 1 0
match_shotgun.hold = off
match_shotgun.use = off
match_shotgun.use_button = z
match_shotgun.pulse = off
match_shotgun.use_pulse = off
match_shotgun.use_linger = 0
match_shotgun.use_rumble_window = 0
match_shotgun.shot = off
match_shotgun.sounds = none
match_shotgun.sound = 0.8 0.3 0.15 both
controls.match_shotgun.stick_c = 0
# 0x38: tommy gun: no sight, the game rumbles every few rounds
tommy_gun.trigger = feedback 0.2 0.3
tommy_gun.rumble = 0 0
tommy_gun.hold = off
tommy_gun.use = off
tommy_gun.use_button = z
tommy_gun.pulse = off
tommy_gun.use_pulse = 1 0.1 0.045 both every 0.085 alternate
tommy_gun.use_linger = 0
tommy_gun.use_rumble_window = 0.3
tommy_gun.shot = off
tommy_gun.sounds = none
tommy_gun.sound = 0.8 0.3 0.15 both
controls.tommy_gun.stick_c = 0
# 0x39: auto rifle: R aims, walks while aiming (told from the flamethrower by its sounds)
auto_rifle.trigger = feedback 0.2 0.25
auto_rifle.rumble = 1 0
auto_rifle.hold = off
auto_rifle.use = off
auto_rifle.use_button = z
auto_rifle.pulse = off
auto_rifle.use_pulse = off
auto_rifle.use_linger = 0
auto_rifle.use_rumble_window = 0
auto_rifle.shot = off
auto_rifle.sounds = none
auto_rifle.sound = 0.8 0.3 0.15 both
controls.auto_rifle.stick_c = 0
# 0x3B: the story's shotgun (Spooky's mansion)
shotgun.trigger = off
shotgun.rumble = 1 0
shotgun.hold = off
shotgun.use = off
shotgun.use_button = z
shotgun.pulse = off
shotgun.use_pulse = off
shotgun.use_linger = 0
shotgun.use_rumble_window = 0
shotgun.shot = off
shotgun.sounds = none
shotgun.sound = 0.8 0.3 0.15 both
controls.shotgun.stick_c = 0
# 0x18: a sighted item, not seen yet
item_18.trigger = off
item_18.rumble = 1 0
item_18.hold = off
item_18.use = off
item_18.use_button = z
item_18.pulse = off
item_18.use_pulse = off
item_18.use_linger = 0
item_18.use_rumble_window = 0
item_18.shot = off
item_18.sounds = none
item_18.sound = 0.8 0.3 0.15 both
controls.item_18.stick_c = 0
# 0x1F: a gun aimed with R, not seen yet
item_1f.trigger = off
item_1f.rumble = 1 0
item_1f.hold = off
item_1f.use = off
item_1f.use_button = z
item_1f.pulse = off
item_1f.use_pulse = off
item_1f.use_linger = 0
item_1f.use_rumble_window = 0
item_1f.shot = off
item_1f.sounds = none
item_1f.sound = 0.8 0.3 0.15 both
controls.item_1f.stick_c = 0
# 0x26: a match's gun, not seen yet
item_26.trigger = off
item_26.rumble = 1 0
item_26.hold = off
item_26.use = off
item_26.use_button = z
item_26.pulse = off
item_26.use_pulse = off
item_26.use_linger = 0
item_26.use_rumble_window = 0
item_26.shot = off
item_26.sounds = none
item_26.sound = 0.8 0.3 0.15 both
controls.item_26.stick_c = 0
# 0x29: a sighted item, not seen yet
item_29.trigger = off
item_29.rumble = 1 0
item_29.hold = off
item_29.use = off
item_29.use_button = z
item_29.pulse = off
item_29.use_pulse = off
item_29.use_linger = 0
item_29.use_rumble_window = 0
item_29.shot = off
item_29.sounds = none
item_29.sound = 0.8 0.3 0.15 both
controls.item_29.stick_c = 0
# 0x3A: a match's gun, not seen yet
item_3a.trigger = off
item_3a.rumble = 1 0
item_3a.hold = off
item_3a.use = off
item_3a.use_button = z
item_3a.pulse = off
item_3a.use_pulse = off
item_3a.use_linger = 0
item_3a.use_rumble_window = 0
item_3a.shot = off
item_3a.sounds = none
item_3a.sound = 0.8 0.3 0.15 both
controls.item_3a.stick_c = 0
# 0x3F: a sighted item, not seen yet
item_3f.trigger = off
item_3f.rumble = 1 0
item_3f.hold = off
item_3f.use = off
item_3f.use_button = z
item_3f.pulse = off
item_3f.use_pulse = off
item_3f.use_linger = 0
item_3f.use_rumble_window = 0
item_3f.shot = off
item_3f.sounds = none
item_3f.sound = 0.8 0.3 0.15 both
controls.item_3f.stick_c = 0
# 0x41: a sighted item, not seen yet
item_41.trigger = off
item_41.rumble = 1 0
item_41.hold = off
item_41.use = off
item_41.use_button = z
item_41.pulse = off
item_41.use_pulse = off
item_41.use_linger = 0
item_41.use_rumble_window = 0
item_41.shot = off
item_41.sounds = none
item_41.sound = 0.8 0.3 0.15 both
controls.item_41.stick_c = 0

# peeing: free up to pee.from, then from pee.min to pee.max with the stream's power
pee.from = 0.2
pee.min = 0.15
pee.max = 0.8

# --- controls ---
controls.match_turn = 6        # degrees a 1/30 s step, the right stick pushed fully (a match)
controls.turret_step = 3       # a tank turret's degrees a 1/30 s step
controls.stick_step = 41       # how far (of 82) the left stick goes before it steps zoom or a C button
controls.mouse_degrees = 0.15  # camera degrees per mouse pixel
controls.mouse_aim = 0.05      # aim per mouse pixel (20 pixels in a frame is the stick pushed fully)
controls.sight_zoom_frames = 6 # frames a wheel step holds zoom in the sight
)";

struct Values {
    std::map<std::string, std::string> text;  // name -> value as written
    std::map<std::string, std::vector<Haptic>> haptics;
    std::map<std::string, TriggerEffect> triggers;
    std::map<std::string, RumbleFeel> rumbles;
    std::map<std::string, float> numbers;
    std::map<std::string, std::string> names;  // item_23 -> magnum
    std::map<std::string, RumbleFeel> holds;    // a weapon's own rumble while in hand
    std::set<std::string> holds_off;            // "off" in the file, over a built-in one
    std::map<std::string, std::string> buttons;  // a weapon's use button: a, b, z or r
    std::map<std::string, std::vector<std::pair<int, int>>> sounds;  // a weapon's sounds that rumble
    std::map<std::string, Pulse> pulses;        // "<weapon>.pulse", "<weapon>.use_pulse"
    std::set<std::string> pulses_off;
};

std::mutex g_mutex;
Values g_values;          // the file over the defaults
Values g_builtin;         // the defaults alone
std::atomic<uint32_t> g_generation{0};
bool g_loaded = false;
time_t g_seen = -1;
std::chrono::steady_clock::time_point g_last_look;

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r");
    if (a == std::string::npos) return {};
    const size_t b = s.find_last_not_of(" \t\r");
    return s.substr(a, b - a + 1);
}

// "name = value  # comment" lines into `out`, later ones winning.
void read_lines(std::istream& in, std::map<std::string, std::string>& out) {
    for (std::string line; std::getline(in, line);) {
        if (const size_t hash = line.find('#'); hash != std::string::npos) line.resize(hash);
        const size_t equals = line.find('=');
        if (equals == std::string::npos) continue;
        const std::string name = trim(line.substr(0, equals));
        const std::string value = trim(line.substr(equals + 1));
        if (!name.empty() && !value.empty()) out[name] = value;
    }
}

bool parse_grip(const std::string& word, Haptic::Grip* grip) {
    if (word == "left") *grip = Haptic::Grip::Left;
    else if (word == "right") *grip = Haptic::Grip::Right;
    else if (word == "both") *grip = Haptic::Grip::Both;
    else if (word == "trigger") *grip = Haptic::Grip::Trigger;
    else return false;
    return true;
}

bool parse_haptics(const std::string& value, std::vector<Haptic>* out) {
    std::stringstream parts(value);
    for (std::string part; std::getline(parts, part, ';');) {
        std::stringstream words(part);
        Haptic h;
        std::string grip;
        if (!(words >> h.intensity >> h.sharpness >> h.duration >> grip) || !parse_grip(grip, &h.grip)) return false;
        out->push_back(h);
    }
    return !out->empty();
}

bool parse_trigger(const std::string& value, TriggerEffect* out) {
    std::stringstream words(value);
    std::string mode;
    words >> mode;
    TriggerEffect e;
    if (mode == "off") {
        *out = e;
        return true;
    }
    if (mode == "feedback") {
        float from = 0, strength = 0;
        if (!(words >> from >> strength)) return false;
        e.mode = TriggerEffect::Mode::Feedback;
        for (int i = 0; i < conker::input::kTriggerZones; i++) {
            e.zones[i] = static_cast<float>(i) >= from * conker::input::kTriggerZones ? strength : 0.0f;
        }
    } else if (mode == "weapon") {
        e.mode = TriggerEffect::Mode::Weapon;
        if (!(words >> e.start >> e.end >> e.strength)) return false;
    } else if (mode == "vibration") {
        e.mode = TriggerEffect::Mode::Vibration;
        if (!(words >> e.start >> e.amplitude >> e.frequency)) return false;
    } else {
        return false;
    }
    *out = e;
    return true;
}

// Every line's typed value; a line that does not read keeps the built-in one (and is reported).
Values typed(const std::map<std::string, std::string>& text, const Values* fallback) {
    Values v;
    v.text = text;
    for (const auto& [name, value] : text) {
        bool ok = true;
        if (name.size() > 5 && name.ends_with(".name")) {
            v.names[name.substr(0, name.size() - 5)] = value;
        } else if (name.size() > 8 && name.ends_with(".trigger")) {
            TriggerEffect e;
            ok = parse_trigger(value, &e);
            if (ok) v.triggers[name.substr(0, name.size() - 8)] = e;
        } else if ((name.size() > 5 && name.ends_with(".hold")) || (name.size() > 4 && name.ends_with(".use"))) {
            // A weapon's hold rumble under its name, its use rumble under "<name>.use".
            const std::string key = name.ends_with(".hold") ? name.substr(0, name.size() - 5) : name;
            std::stringstream words(value);
            RumbleFeel f;
            if (value == "off") {
                v.holds.erase(key);
                v.holds_off.insert(key);
            } else {
                ok = static_cast<bool>(words >> f.intensity >> f.sharpness);
                if (ok) v.holds[key] = f;
            }
        } else if ((name.size() > 6 && name.ends_with(".pulse")) || (name.size() > 10 && name.ends_with(".use_pulse"))) {
            // "<intensity> <sharpness> <seconds> <grip> every <seconds> [alternate]", or "off"
            if (value == "off") {
                v.pulses_off.insert(name);
            } else {
                std::stringstream words(value);
                Pulse pulse;
                std::string grip, every, alternate;
                ok = static_cast<bool>(words >> pulse.haptic.intensity >> pulse.haptic.sharpness >> pulse.haptic.duration >> grip >>
                                       every >> pulse.every) &&
                     parse_grip(grip, &pulse.haptic.grip) && every == "every" && pulse.every > 0.01f;
                if (ok && (words >> alternate)) {
                    ok = alternate == "alternate";
                    pulse.alternate = ok;
                }
                if (ok) v.pulses[name] = pulse;
            }
        } else if (name.size() > 5 && name.ends_with(".shot")) {
            // "<intensity> <sharpness> <seconds> <grip> [alternate]", or "off"
            if (value == "off") {
                v.pulses_off.insert(name);
            } else {
                std::stringstream words(value);
                Pulse shot;
                std::string grip, alternate;
                ok = static_cast<bool>(words >> shot.haptic.intensity >> shot.haptic.sharpness >> shot.haptic.duration >> grip) &&
                     parse_grip(grip, &shot.haptic.grip);
                if (ok && (words >> alternate)) {
                    ok = alternate == "alternate";
                    shot.alternate = ok;
                }
                if (ok) v.pulses[name] = shot;
            }
        } else if (name.size() > 7 && name.ends_with(".sounds")) {
            // "1092-1096 1547", or "none"
            std::vector<std::pair<int, int>> ranges;
            if (value != "none") {
                std::stringstream words(value);
                for (std::string word; words >> word;) {
                    const size_t dash = word.find('-');
                    char* end = nullptr;
                    const long lo = std::strtol(word.c_str(), &end, 10);
                    const long hi = dash == std::string::npos ? lo : std::strtol(word.c_str() + dash + 1, nullptr, 10);
                    if (end == word.c_str() || hi < lo) {
                        ok = false;
                        break;
                    }
                    ranges.emplace_back(static_cast<int>(lo), static_cast<int>(hi));
                }
            }
            if (ok) v.sounds[name.substr(0, name.size() - 7)] = ranges;
        } else if (name.size() > 6 && name.ends_with(".sound")) {
            std::vector<Haptic> parts;
            ok = parse_haptics(value, &parts);
            if (ok) v.haptics[name] = parts;
        } else if (name.size() > 11 && name.ends_with(".use_button")) {
            // one or more of a, b, z, r ("z b": either counts)
            std::stringstream words(value);
            std::string all;
            for (std::string word; words >> word;) {
                if (word != "a" && word != "b" && word != "z" && word != "r") {
                    ok = false;
                    break;
                }
                all += word;
            }
            ok = ok && !all.empty();
            if (ok) v.buttons[name.substr(0, name.size() - 11)] = all;
        } else if (name.size() > 7 && name.ends_with(".rumble")) {
            std::stringstream words(value);
            RumbleFeel f;
            ok = static_cast<bool>(words >> f.intensity >> f.sharpness);
            if (ok) v.rumbles[name.substr(0, name.size() - 7)] = f;
        } else if (name.find('.') != std::string::npos) {
            char* end = nullptr;
            const float n = std::strtof(value.c_str(), &end);
            ok = end != value.c_str();
            if (ok) v.numbers[name] = n;
        } else {
            std::vector<Haptic> parts;
            ok = parse_haptics(value, &parts);
            if (ok) v.haptics[name] = parts;
        }
        if (!ok) {
            std::printf("[conker] feel.cfg: cannot read \"%s = %s\"; keeping the built-in value\n", name.c_str(), value.c_str());
        }
    }
    if (fallback != nullptr) {
        for (const auto& [k, x] : fallback->haptics) v.haptics.try_emplace(k, x);
        for (const auto& [k, x] : fallback->triggers) v.triggers.try_emplace(k, x);
        for (const auto& [k, x] : fallback->rumbles) v.rumbles.try_emplace(k, x);
        for (const auto& [k, x] : fallback->numbers) v.numbers.try_emplace(k, x);
        for (const auto& [k, x] : fallback->buttons) v.buttons.try_emplace(k, x);
        for (const auto& [k, x] : fallback->sounds) v.sounds.try_emplace(k, x);
        for (const auto& [k, x] : fallback->pulses) {
            if (!v.pulses_off.contains(k)) v.pulses.try_emplace(k, x);
        }
        for (const auto& [k, x] : fallback->holds) {
            if (!v.holds_off.contains(k)) v.holds.try_emplace(k, x);
        }
    }
    return v;
}

std::string path() { return (conker::data_folder() / "feel.cfg").string(); }

// Under g_mutex.
void load_locked() {
    if (!g_loaded) {
        std::map<std::string, std::string> text;
        std::stringstream defaults(kDefaults);
        read_lines(defaults, text);
        g_builtin = typed(text, nullptr);
        g_values = g_builtin;
        g_loaded = true;
    }
    // Read once, unless live: then looked at again every half second.
    const auto now = std::chrono::steady_clock::now();
    if (g_seen != -1 && (!live() || now - g_last_look < std::chrono::milliseconds(500))) return;
    g_last_look = now;
    struct stat info {};
    if (stat(path().c_str(), &info) != 0) {
        if (g_seen == -1) {
            std::ofstream out(path());
            out << kDefaults;
            std::printf("[conker] feel.cfg: written with the built-in values (%s)\n", path().c_str());
            g_seen = 0;
        }
        return;
    }
    if (info.st_mtime == g_seen) return;
    g_seen = info.st_mtime;
    std::map<std::string, std::string> text;
    {
        std::ifstream in(path());
        read_lines(in, text);
    }
    // A file from an older build gets the lines it lacks (new weapons, say), with their built-in values;
    // what it has stays as it is.
    std::string missing;
    for (const auto& [name, value] : g_builtin.text) {
        if (!text.contains(name)) missing += name + " = " + value + "\n";
    }
    if (!missing.empty()) {
        std::ofstream out(path(), std::ios::app);
        out << "\n# --- added by a newer build, with their built-in values ---\n" << missing;
        out.close();
        std::stringstream added(missing);
        read_lines(added, text);
        struct stat again {};
        if (stat(path().c_str(), &again) == 0) g_seen = again.st_mtime;
        std::printf("[conker] feel.cfg: added the lines it lacked\n");
    }
    g_values = typed(text, &g_builtin);
    g_generation.fetch_add(1);
    std::printf("[conker] feel.cfg: read (%zu lines)\n", text.size());
    std::fflush(stdout);
}

}  // namespace

bool live() {
    static const bool on = [] {
        const char* value = std::getenv("CONKER_FEEL_LIVE");
        const bool set = value != nullptr && value[0] == '1';
        if (set) std::printf("[conker] feel.cfg: live (read again as it is saved), weapon survey on\n");
        return set;
    }();
    return on;
}

void refresh() {
    std::lock_guard lock(g_mutex);
    load_locked();
}

uint32_t generation() { return g_generation.load(); }

std::vector<Haptic> haptic(const char* name) {
    std::lock_guard lock(g_mutex);
    load_locked();
    const auto it = g_values.haptics.find(name);
    return it != g_values.haptics.end() ? it->second : std::vector<Haptic>{};
}

TriggerEffect trigger(const char* name) {
    std::lock_guard lock(g_mutex);
    load_locked();
    const auto it = g_values.triggers.find(name);
    return it != g_values.triggers.end() ? it->second : TriggerEffect{};
}

RumbleFeel rumble(const char* name) {
    std::lock_guard lock(g_mutex);
    load_locked();
    const auto it = g_values.rumbles.find(name);
    return it != g_values.rumbles.end() ? it->second : RumbleFeel{};
}

struct Weapon {
    uint8_t item;
    const char* name;
};
// The weapons by the item a player holds (player block +0x78), as the weapon survey and a play-through
// of every MULTI mode found them (docs/enhancements.md).
constexpr Weapon kWeapons[] = {
    {0x02, "slingshot"},
    {0x09, "uzis"},
    {0x0C, "story_launcher"},
    {0x14, "knives"},
    {0x15, "flamethrower"},
    {0x16, "rocket_launcher"},
    {0x19, "sword_or_bat"},
    {0x1B, "mounted_gun_item"},
    {0x21, "bat_or_sword"},
    {0x22, "chainsaw"},
    {0x23, "revolver"},
    {0x24, "sniper"},
    {0x37, "match_shotgun"},
    {0x38, "tommy_gun"},
    {0x39, "auto_rifle"},
    {0x3B, "shotgun"},
};

std::string weapon_key(uint8_t item) {
    for (const Weapon& weapon : kWeapons) {
        if (weapon.item == item) return weapon.name;
    }
    char id[16];
    std::snprintf(id, sizeof(id), "item_%02x", item);
    return id;
}

std::string weapon_name(uint8_t item) { return name(weapon_key(item)); }

bool hold(const std::string& weapon, RumbleFeel* feel) {
    std::lock_guard lock(g_mutex);
    load_locked();
    const auto it = g_values.holds.find(weapon);
    if (it == g_values.holds.end()) return false;
    *feel = it->second;
    return true;
}

bool shot(const std::string& weapon, Pulse* out) {
    std::lock_guard lock(g_mutex);
    load_locked();
    const auto it = g_values.pulses.find(weapon + ".shot");
    if (it == g_values.pulses.end()) return false;
    *out = it->second;
    return true;
}

bool pulse(const std::string& weapon, bool used, Pulse* out) {
    std::lock_guard lock(g_mutex);
    load_locked();
    auto it = used ? g_values.pulses.find(weapon + ".use_pulse") : g_values.pulses.end();
    if (it == g_values.pulses.end()) it = g_values.pulses.find(weapon + ".pulse");
    if (it == g_values.pulses.end()) return false;
    *out = it->second;
    return true;
}

std::vector<Haptic> sound_haptic(const std::string& weapon, int sound) {
    std::lock_guard lock(g_mutex);
    load_locked();
    const auto it = g_values.sounds.find(weapon);
    if (it == g_values.sounds.end()) return {};
    for (const auto& [lo, hi] : it->second) {
        if (sound >= lo && sound <= hi) {
            const auto h = g_values.haptics.find(weapon + ".sound");
            return h != g_values.haptics.end() ? h->second : std::vector<Haptic>{};
        }
    }
    return {};
}

bool use(const std::string& weapon, RumbleFeel* feel, char* button) {
    std::lock_guard lock(g_mutex);
    load_locked();
    const auto it = g_values.holds.find(weapon + ".use");
    if (it == g_values.holds.end()) return false;
    *feel = it->second;
    const auto b = g_values.buttons.find(weapon);
    *button = b != g_values.buttons.end() ? b->second[0] : 'z';
    return true;
}

uint16_t use_buttons(const std::string& weapon) {
    std::lock_guard lock(g_mutex);
    load_locked();
    const auto b = g_values.buttons.find(weapon);
    const std::string letters = b != g_values.buttons.end() ? b->second : "z";
    uint16_t mask = 0;
    for (char c : letters) {
        mask |= c == 'a' ? conker::input::kA : c == 'b' ? conker::input::kB : c == 'r' ? conker::input::kR : conker::input::kZ;
    }
    return mask;
}

std::string name(const std::string& id) {
    std::lock_guard lock(g_mutex);
    load_locked();
    const auto it = g_values.names.find(id);
    return it != g_values.names.end() ? it->second : id;
}

float number(const char* name) {
    std::lock_guard lock(g_mutex);
    load_locked();
    const std::string key = name;
    // Online, every machine must turn and aim alike: the built-in control numbers.
    const Values& from = key.starts_with("controls.") && conker::net::active() ? g_builtin : g_values;
    const auto it = from.numbers.find(key);
    return it != from.numbers.end() ? it->second : 0.0f;
}

}  // namespace conker::feel
