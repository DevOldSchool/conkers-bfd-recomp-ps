// The players as the pad's feel finds them in game memory: Conker in the story, the four players of a
// multiplayer match, each player's block and what it holds. Shared by the haptics, the adaptive
// triggers and the weapon survey.
//
// In a match (D_800BE616 nonzero) player n is actor n (D_800CC2D0, 0x32C bytes each, as the match's
// own tallies walk them) and plays on controller n + 1. In the story Conker (*D_80088720) plays on
// controller 1.
#pragma once

#include <cstdint>
#include <string>

#include "recomp.h"

#include "enhancements/feel/feel.hpp"

namespace conker::players {

inline gpr kseg0(uint32_t address) { return static_cast<gpr>(static_cast<int32_t>(address)); }

constexpr uint32_t kConker = 0x80088720;       // D_80088720: Conker's actor
constexpr uint32_t kWorldPaused = 0x800BEAC0;  // D_800BEAC0: the world frame is skipped (pause, menus)
constexpr uint32_t kMultiplayer = 0x800BE616;  // D_800BE616: nonzero during a multiplayer match
constexpr uint32_t kScene = 0x800BE9F0;        // D_800BE9F0: the scene being played
constexpr uint32_t kActors = 0x800CC2D0;       // D_800CC2D0: the actors; player n is actor n in a match
constexpr uint32_t kActorSize = 0x32C;

constexpr uint32_t kActorModel = 0x4;          // u8: 0 for Conker
constexpr uint32_t kActorPosition = 0x14;      // 3 floats
constexpr uint32_t kActorAnimation = 0x84;     // u16
constexpr uint32_t kActorHealth = 0x1CA;       // u8
constexpr uint32_t kActorPlayer = 0x31C;       // the player's block

constexpr uint32_t kPlayerAction = 0x75;       // u8: the B pad's action
constexpr uint32_t kPlayerItem = 0x78;         // u8: the item held
constexpr uint32_t kPlayerItemState = 0x11A;   // u8: kItemHeld while it is in hand
constexpr uint32_t kPlayerSight = 0x197;       // u8: kMountedGun through a sight or on a mounted gun
constexpr uint32_t kPlayerLook = 0x198;        // u8: R's look, 1 going in, 2 looking, 3 coming out
constexpr uint32_t kPlayerGunRounds = 0x1AA;   // s16: the mounted gun's rounds fired

constexpr uint8_t kItemHeld = 2;
constexpr uint8_t kMountedGun = 2;
constexpr int kPlayers = 4;

inline bool in_rdram(uint32_t address) { return address >= 0x80000000u && address < 0x80800000u; }

inline bool multiplayer(uint8_t* rdram) { return MEM_BU(0, kseg0(kMultiplayer)) != 0; }

// False while the world frame is skipped (the pause menu, other menus).
inline bool world_running(uint8_t* rdram) { return MEM_BU(0, kseg0(kWorldPaused)) == 0; }

// The actor player `port` plays, or 0: Conker for port 0 in the story, actor n for port n in a match
// (one with a player's block).
inline uint32_t actor(uint8_t* rdram, int port) {
    if (!multiplayer(rdram)) {
        const uint32_t conker = static_cast<uint32_t>(MEM_W(0, kseg0(kConker)));
        return port == 0 && in_rdram(conker) ? conker : 0;
    }
    const uint32_t actor = kActors + static_cast<uint32_t>(port) * kActorSize;
    return in_rdram(static_cast<uint32_t>(MEM_W(kActorPlayer, kseg0(actor)))) ? actor : 0;
}

// The actor whose weapon port `port` feels, or 0: as actor(), but only while the world runs, and in
// the story only while it is Conker himself (model 0).
inline uint32_t armed_actor(uint8_t* rdram, int port) {
    if (!world_running(rdram)) return 0;
    const uint32_t found = actor(rdram, port);
    if (found != 0 && !multiplayer(rdram) && MEM_BU(kActorModel, kseg0(found)) != 0) return 0;
    return found;
}

inline uint16_t animation(uint8_t* rdram, uint32_t actor) { return actor != 0 ? MEM_HU(kActorAnimation, kseg0(actor)) : 0; }

// An actor's player block, or 0.
inline uint32_t block(uint8_t* rdram, uint32_t actor) {
    if (actor == 0) return 0;
    const uint32_t player = static_cast<uint32_t>(MEM_W(kActorPlayer, kseg0(actor)));
    return in_rdram(player) ? player : 0;
}

// Seated at a mounted machine gun.
inline bool mounted(uint8_t* rdram, uint32_t block) { return block != 0 && MEM_BU(kPlayerSight, kseg0(block)) == kMountedGun; }

// The feel.cfg name of the weapon in a player's hand, or "" with nothing in hand.
inline std::string held_weapon(uint8_t* rdram, uint32_t block) {
    if (block == 0 || MEM_BU(kPlayerItemState, kseg0(block)) != kItemHeld) return {};
    return conker::feel::weapon_name(MEM_BU(kPlayerItem, kseg0(block)));
}

}  // namespace conker::players
