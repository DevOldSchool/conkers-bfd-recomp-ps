// More bots with three or four players in a multiplayer match. Called once a game frame from
// src/runtime/frame_input.cpp. CONKER_MORE_BOTS=0 keeps the game's own limits.
//
// The bots page after the join window (func_151E1214) lets the players add bots up to a limit on
// the characters in the match, players and bots together, read from a row of D_800AB74C: one byte
// for each number of players, 1 to 4. The game mode's record (D_800AB690, 10 bytes a mode, byte 1)
// names a setting whose value (0 or 1) picks the row. The rows are {10, 8, 4, 4} and {8, 4, 4, 4},
// so three players could add one bot and four none: on the console four views of the same scene
// had to draw every character four times. Heist's teams are filled from the same total, which is why
// three players had a team each. Here every row allows its one-player total whatever the number of
// players: {10, 10, 10, 10} and {8, 8, 8, 8}.
//
// Team modes set the team size on that page instead (func_151E1744): players fill the slots in
// order, team 0's first, so players share a team when teams are big enough. The mode's team record
// (D_800AB710, 12 bytes: the number of teams, then three limits for 1 and 2 players and three for 3
// and 4) let 3 and 4 players have teams of one: Heist's four teams were {0, 8, 2} characters and
// players per team with two players but {0, 4, 1} with more. Here 3 and 4 players get the two-player
// limits, and Heist's teams hold up to four, so every player can be on one team.
//
// The join screen has its own limit: RACE (mode type 7) let only two players join, and drew two
// slots in the middle. The conker_more_bots_join_* hooks give it the other modes' four slots.
#include "enhancements/more_bots/more_bots.hpp"

#include <atomic>
#include <cstdlib>

#include "recomp.h"
#include "enhancements/status.hpp"
#include "enhancements/switches.hpp"

namespace {

inline gpr kseg0(uint32_t address) { return static_cast<gpr>(static_cast<int32_t>(address)); }

constexpr uint32_t kLimits = 0x800AB74C;  // D_800AB74C: s8[2][4], characters allowed by players
constexpr int kRows = 2;
constexpr int kPlayers = 4;
constexpr int8_t kGame[kRows][kPlayers] = {{10, 8, 4, 4}, {8, 4, 4, 4}};

constexpr uint32_t kTeamRecords = 0x800AB710;  // D_800AB710: 12 bytes a team mode
constexpr int kTeamRecordCount = 5;
constexpr int kTeamRecordSize = 12;
constexpr int8_t kTeamGame[kTeamRecordCount][kTeamRecordSize] = {
    {4, 0, 8, 2, 0, 4, 1, 0, 0, 1, 2, 3},     // four teams (Heist)
    {2, 4, 8, 2, 4, 4, 2, 0, 0, 1, -1, -1},
    {2, 0, 12, 8, 0, 12, 8, 2, 9, 5, -1, -1},
    {2, 0, 12, 8, 0, 4, 2, 0, 4, 5, -1, -1},
    {2, 0, 12, 8, 0, 8, 2, 2, 7, 8, -1, -1},
};

// A team record as MORE BOTS has it: the 1-2 player limits for every number of players, and four
// teams of up to four.
int8_t more_team(int record, int i) {
    if (record == 0 && (i == 2 || i == 5)) return 16;
    if (record == 0 && (i == 3 || i == 6)) return 4;
    if (i >= 4 && i <= 6) return kTeamGame[record][i - 3];
    return kTeamGame[record][i];
}

std::atomic<bool> g_enabled{[] {
    const char* value = std::getenv("CONKER_MORE_BOTS");
    return value == nullptr || value[0] != '0';
}()};  // the debug menu switches it

bool more_bots_enabled() { return g_enabled.load(std::memory_order_relaxed); }

}  // namespace

// Once a game frame, so the switch acts at once, and online after the host's settings are applied,
// on every machine of the session alike.
void conker::more_bots::frame(uint8_t* rdram) {
    const bool on = more_bots_enabled();
    for (int row = 0; row < kRows; row++) {
        for (int players = 0; players < kPlayers; players++) {
            MEM_B(row * kPlayers + players, kseg0(kLimits)) = on ? kGame[row][0] : kGame[row][players];
        }
    }
    for (int record = 0; record < kTeamRecordCount; record++) {
        for (int i = 1; i <= 6; i++) {
            MEM_B(record * kTeamRecordSize + i, kseg0(kTeamRecords)) = on ? more_team(record, i) : kTeamGame[record][i];
        }
    }
}

// Hooked in func_151E0B70 once the join screen has chosen its player limit (s8 at sp + 0x33): two
// for RACE, four for the rest.
extern "C" void conker_more_bots_join_limit(uint8_t* rdram, recomp_context* ctx) {
    if (more_bots_enabled()) {
        MEM_B(0x33, ctx->r29) = 4;
    }
}

// Hooked in func_151EC648, the join screen's drawing, once it has chosen how many slots to draw
// (s8 at sp + 0xF7) and the first one's x (s5): two from 0x70 for RACE, four from 0x28 otherwise.
extern "C" void conker_more_bots_join_slots(uint8_t* rdram, recomp_context* ctx) {
    if (more_bots_enabled()) {
        MEM_B(0xF7, ctx->r29) = 4;
        ctx->r21 = 0x28;
    }
}

constexpr int kRaceType = 7;  // D_8008FDD4 + 0x2C, the mode's type

// Hooked in func_151E7F60, which places a player's figure on the join screen, once it has chosen the
// slot (v1): the port, one further right for RACE. Without that shift RACE uses all four slots.
extern "C" void conker_more_bots_join_slot(uint8_t* rdram, recomp_context* ctx) {
    const gpr settings = static_cast<gpr>(static_cast<int32_t>(MEM_W(0, kseg0(0x8008FDD4))));
    if (more_bots_enabled() && MEM_B(0x2C, settings) == kRaceType) {
        ctx->r3 = MEM_W(0x50, ctx->r29);  // the port, a0 as saved on entry
    }
}

nlohmann::json conker::status::more_bots() {
    return enhancement("more_bots", "As many bots with three or four players as with one", more_bots_enabled(),
                       "CONKER_MORE_BOTS");
}

conker::switches::Switch conker::switches::more_bots() {
    return {"MORE BOTS", more_bots_enabled, [](bool on) { g_enabled.store(on); }};
}
