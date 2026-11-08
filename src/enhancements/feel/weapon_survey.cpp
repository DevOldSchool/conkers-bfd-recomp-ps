// The weapon survey; see weapon_survey.hpp.
#include "enhancements/feel/weapon_survey.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>

#include "recomp.h"

#include "enhancements/feel/feel.hpp"
#include "platform/data_folder.hpp"
#include "platform/input.hpp"
#include "runtime/frame_input.hpp"

namespace conker::survey {

namespace {

inline gpr kseg0(uint32_t address) { return static_cast<gpr>(static_cast<int32_t>(address)); }

constexpr uint32_t kConker = 0x80088720;       // D_80088720: Conker's actor
constexpr uint32_t kMultiplayer = 0x800BE616;  // D_800BE616: nonzero during a match
constexpr uint32_t kScene = 0x800BE9F0;        // D_800BE9F0: the scene being played
constexpr uint32_t kActors = 0x800CC2D0;       // D_800CC2D0: player n is actor n in a match
constexpr uint32_t kActorSize = 0x32C;
constexpr uint32_t kActorPosition = 0x14;      // floats
constexpr uint32_t kActorAnimation = 0x84;     // u16
constexpr uint32_t kActorPlayer = 0x31C;       // the player's block
constexpr uint32_t kPlayerAction = 0x75;       // u8: the B pad's action
constexpr uint32_t kPlayerItem = 0x78;         // u8
constexpr uint32_t kPlayerItemState = 0x11A;   // u8: 2 in hand
constexpr uint32_t kPlayerSight = 0x197;       // u8: 2 through a sight or on a mounted gun
constexpr uint32_t kPlayerLook = 0x198;        // u8: R's look, 1 going in, 2 looking, 3 coming out
constexpr int kPlayers = 4;
constexpr size_t kMaxListed = 48;              // animations and sounds kept per item

struct Item {
    std::set<uint32_t> scenes;
    bool story = false;
    bool match = false;
    uint64_t frames = 0;
    std::set<uint8_t> item_states;
    std::set<uint8_t> sight_states;
    std::set<uint8_t> look_states;
    std::set<uint8_t> actions;
    uint64_t aimed_frames = 0;
    float moved_aimed = 0.0f;     // distance walked while aimed (sight or R's look)
    float moved = 0.0f;           // and in all
    std::set<uint16_t> animations;
    std::map<uint16_t, int> sounds;
    int rumbles = 0;              // the game's own rumble coming on while it was held
    std::map<std::string, int> presses;  // N64 buttons pressed (counted as they go down) while held
};

std::mutex g_mutex;
std::map<uint8_t, Item> g_items;
bool g_dirty = false;
std::chrono::steady_clock::time_point g_last_write;
uint8_t g_held[kPlayers] = {};         // the item each player holds now (0: none)
bool g_holding[kPlayers] = {};
float g_last_pos[kPlayers][3] = {};
bool g_rumbling[kPlayers] = {};

bool in_rdram(uint32_t address) { return address >= 0x80000000u && address < 0x80800000u; }

float read_float(uint8_t* rdram, uint32_t address) {
    const uint32_t word = static_cast<uint32_t>(MEM_W(0, kseg0(address)));
    float value;
    std::memcpy(&value, &word, sizeof(value));
    return value;
}

void write_file() {
    const std::string path = (conker::data_folder() / "weapon_survey.txt").string();
    std::ofstream out(path);
    out << "# Weapon survey: each item held this run (see src/enhancements/feel/weapon_survey.hpp).\n"
           "# sight: +0x197 states (2 through a sight or on a mounted gun); look: +0x198 (R's look, 2 looking)\n"
           "# walked while aimed: distance moved while sighted or looking; sounds: id x times started\n\n";
    for (const auto& [id, item] : g_items) {
        const std::string name = conker::feel::weapon_name(id);
        out << "item 0x" << std::hex << int(id) << std::dec << " (" << name << ")\n";
        out << "  where:";
        for (uint32_t scene : item.scenes) out << " scene " << scene;
        out << (item.story ? " story" : "") << (item.match ? " match" : "") << "\n";
        out << "  held " << item.frames << " frames, aimed " << item.aimed_frames << "\n";
        auto list = [&](const char* what, const std::set<uint8_t>& values) {
            out << "  " << what << ":";
            for (uint8_t v : values) out << " " << int(v);
            out << "\n";
        };
        list("item states", item.item_states);
        list("sight", item.sight_states);
        list("look", item.look_states);
        list("B pad action", item.actions);
        char moved[96];
        std::snprintf(moved, sizeof(moved), "  walked while aimed %.0f (of %.0f walked)\n", item.moved_aimed, item.moved);
        out << moved;
        out << "  animations:";
        for (uint16_t a : item.animations) {
            char text[12];
            std::snprintf(text, sizeof(text), " 0x%X", a);
            out << text;
        }
        out << "\n  sounds:";
        for (const auto& [s, n] : item.sounds) out << " " << s << "x" << n;
        out << "\n  buttons pressed:";
        for (const auto& [b, n] : item.presses) out << " " << b << "x" << n;
        out << "\n  game rumble came on " << item.rumbles << " times\n\n";
    }
    out.flush();
    g_dirty = false;
    g_last_write = std::chrono::steady_clock::now();
}

// The players and their actors: Conker alone in the story, the four in a match.
uint32_t player_actor(uint8_t* rdram, int player, bool match) {
    if (!match) {
        const uint32_t conker = static_cast<uint32_t>(MEM_W(0, kseg0(kConker)));
        return player == 0 && in_rdram(conker) ? conker : 0;
    }
    return kActors + static_cast<uint32_t>(player) * kActorSize;
}

}  // namespace

void frame(uint8_t* rdram) {
    if (!conker::feel::live()) return;
    const bool match = MEM_BU(0, kseg0(kMultiplayer)) != 0;
    const uint32_t scene = static_cast<uint32_t>(MEM_W(0, kseg0(kScene)));
    std::lock_guard lock(g_mutex);
    for (int p = 0; p < kPlayers; p++) {
        const uint32_t actor = player_actor(rdram, p, match);
        const uint32_t block = actor != 0 ? static_cast<uint32_t>(MEM_W(kActorPlayer, kseg0(actor))) : 0;
        const bool rumbling = conker::input::rumble_on(p);
        if (!in_rdram(block)) {
            g_holding[p] = false;
            g_rumbling[p] = rumbling;
            continue;
        }
        const uint8_t id = MEM_BU(kPlayerItem, kseg0(block));
        const uint8_t state = MEM_BU(kPlayerItemState, kseg0(block));
        const uint8_t sight = MEM_BU(kPlayerSight, kseg0(block));
        // Held, or on a mounted gun (its item is 0x1B but not "in hand").
        const bool holding = id != 0 && (state == 2 || sight == 2);
        float pos[3];
        for (int i = 0; i < 3; i++) pos[i] = read_float(rdram, actor + kActorPosition + 4 * i);
        if (holding) {
            Item& item = g_items[id];
            const size_t before = item.scenes.size() + item.item_states.size() + item.sight_states.size() +
                                  item.look_states.size() + item.animations.size() + item.actions.size();
            item.scenes.insert(scene);
            (match ? item.match : item.story) = true;
            item.frames++;
            item.item_states.insert(state);
            item.sight_states.insert(sight);
            const uint8_t look = MEM_BU(kPlayerLook, kseg0(block));
            item.look_states.insert(look);
            item.actions.insert(MEM_BU(kPlayerAction, kseg0(block)));
            if (item.animations.size() < kMaxListed) item.animations.insert(MEM_HU(kActorAnimation, kseg0(actor)));
            if (g_holding[p] && g_held[p] == id) {
                const float dx = pos[0] - g_last_pos[p][0], dy = pos[1] - g_last_pos[p][1], dz = pos[2] - g_last_pos[p][2];
                const float step = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (step < 200.0f) {  // not a teleport or a respawn
                    item.moved += step;
                    const bool aimed = sight != 0 || look == 1 || look == 2;
                    if (aimed) {
                        item.aimed_frames++;
                        item.moved_aimed += step;
                    }
                }
            }
            if (rumbling && !g_rumbling[p]) item.rumbles++;
            static uint16_t last_buttons[kPlayers] = {};
            const uint16_t buttons = conker::frame_input::port(p).buttons;
            const uint16_t down = buttons & ~last_buttons[p];
            last_buttons[p] = buttons;
            struct Named { uint16_t bit; const char* name; };
            static constexpr Named kNames[] = {
                {conker::input::kA, "A"}, {conker::input::kB, "B"}, {conker::input::kZ, "Z"}, {conker::input::kR, "R"},
            };
            for (const Named& n : kNames) {
                if (down & n.bit) {
                    item.presses[n.name]++;
                    g_dirty = true;
                }
            }
            const size_t after = item.scenes.size() + item.item_states.size() + item.sight_states.size() +
                                 item.look_states.size() + item.animations.size() + item.actions.size();
            if (after != before || !g_holding[p] || g_held[p] != id) g_dirty = true;
            if (!g_holding[p] || g_held[p] != id) {
                std::printf("[conker] survey: player %d holds item 0x%02X (%s), scene %u%s\n", p + 1, id,
                            conker::feel::weapon_name(id).c_str(), scene, match ? ", a match" : "");
            }
        }
        g_held[p] = holding ? id : 0;
        g_holding[p] = holding;
        std::memcpy(g_last_pos[p], pos, sizeof(pos));
        g_rumbling[p] = rumbling;
    }
    // Counts (frames, distance) change all the time: written with the rest, every 2 s at most.
    if (g_dirty || (!g_items.empty() && std::chrono::steady_clock::now() - g_last_write > std::chrono::seconds(10))) {
        if (std::chrono::steady_clock::now() - g_last_write > std::chrono::seconds(2)) write_file();
    }
}

void sound(uint8_t* /*rdram*/, uint16_t sound) {
    if (!conker::feel::live()) return;
    std::lock_guard lock(g_mutex);
    for (int p = 0; p < kPlayers; p++) {
        if (!g_holding[p]) continue;
        Item& item = g_items[g_held[p]];
        auto it = item.sounds.find(sound);
        if (it != item.sounds.end()) {
            it->second++;
        } else if (item.sounds.size() < kMaxListed) {
            item.sounds[sound] = 1;
            g_dirty = true;
        }
    }
}

}  // namespace conker::survey
