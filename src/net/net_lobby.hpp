// The online lobby's network side (docs/online-multiplayer-plan.md, M5): hosting and joining a lobby,
// finding games on the local network, and starting the session. The window that shows it is
// src/net/lobby_ui_rt64.cpp; the session itself is net/net_play.
//
// The lobby runs while the game plays normally underneath. HOST listens on the game's UDP port
// (37800 by default), asks the router to open it (net/net_portmap) and shows join codes
// (net/net_codes). JOIN takes an address or a code, or a game from the LAN list. Every machine
// in the lobby shows the players with their pads, ping and whether their build and ROM match, and
// the host's settings. When every guest is READY the host can START: each machine then starts
// itself again into the session with the same options the command line takes (--net-host,
// --net-join with the port the lobby gave it), so the session begins from a fresh boot as before.
//
// A game whose session has already started still answers: the LAN list shows it as "playing", and
// JOIN on it starts this machine straight into the session, which takes it in from a snapshot of
// the host's game (a free port plays from the next match's join screen; see net_play.cpp).
//
// The wire: UDP datagrams starting with a magic and a type: "CKL1" for PROBE and OFFER, in the clear;
// "CKL3" for the rest, sealed on the link between a guest and the host (net_seal.hpp: the guest asks
// for the host's key first, and its JOIN comes in a FIRST).
//     PROBE    anyone -> a host        is a game here? (to the game port, or broadcast to the
//                                      discovery port 37799, which every host on the network answers)
//     OFFER    host -> prober          its name, build, OS, ROM, game port, lobby or playing, players
//     JOIN     guest -> lobby host     name, pad, build, OS, ROM, the lobby's version (refused if not the host's)
//     REJECT   lobby host -> guest     why not, in plain words
//     STATE    lobby host -> guests    the players, the settings, an echo for the ping (every 250 ms)
//     HEARTBEAT guest -> lobby host    ready, its ping, a time to echo (every 250 ms)
//     START    lobby host -> guest     the session: its port, the number of players
//     GO       guest -> lobby host     START seen; starting again
//     LEAVE    either way              gone (a guest leaving, or the host closing the lobby)
#pragma once

#include <cstdint>
#include <string>
#include <vector>


#include "net/net_portmap.hpp"
#include "net/net_socket.hpp"

namespace conker::net::lobby {

constexpr uint16_t kDiscoveryPort = 37799;

enum class Mode { Idle, Hosting, Probing, Joined, Starting };

struct Player {
    std::string name;
    std::string pad;        // the pad playing: its make and model ("DualSense", "Keyboard"); this machine's own by its name
    int ping = -1;          // ms, -1 not yet known (0 for the host itself)
    bool ready = false;
    bool host = false;
    bool you = false;
};

struct Game {
    std::string name;
    std::string address;    // address:port, for JOIN
    int players = 0;
    bool playing = false;   // its session has started (JOIN takes a free port, or a dropped one)
    bool joinable = true;
    std::string problem;    // why this machine cannot join it ("" if it can)
};

struct View {
    Mode mode = Mode::Idle;
    std::vector<Player> players;     // the host first
    std::vector<Game> games;         // on the local network, newest answers first
    std::string settings;            // the host's settings, one "LABEL on/off" a line
    std::string settings_differ;     // the host's settings that differ from this machine's own
    std::string error;               // the last thing that went wrong, in plain words ("" for none)
    std::string notice;              // the last thing that happened ("" for none)
    std::vector<std::pair<std::string, std::string>> codes;  // (what it is for, join code)
    portmap::Status port;            // hosting: the router's port
    uint16_t game_port = 0;
    std::string key_check;           // hosting: this machine's key check; joined: the host's (net_keys.hpp)
    bool can_start = false;          // hosting: a guest is here and every guest is ready
    bool ready = false;              // joined: this machine's READY
};

// The player name shown to others (conker-recomp-data/online.txt; "Player" until one is typed).
std::string name();
void set_name(const std::string& name);
// The last address or code joined, for the JOIN field.
std::string last_join();

// Host a lobby on `port`. False, with View::error, if the port is taken.
bool host(uint16_t port);
// Look for games on the local network (until leave()); the View's games fill in.
void browse();
// Join the game at `target` (an address, address:port or join code). The answer comes in the View.
void join(const std::string& target);
void set_ready(bool ready);
// The host starts the session: every machine starts itself again into it.
void start();
// Hosting: remove a player (by its place in View::players, 1 for the first guest); its machine cannot
// join this lobby again.
void kick(size_t player);
// Leave the lobby (the host closes it, and the router's port with it).
void leave();

View view();

// For the session (net/net_play.cpp), whose game socket also gets PROBEs: whether a datagram
// is one, answered on `socket`. And the discovery port's answers for a running session.
struct Offer {
    std::string name;
    uint16_t port = 0;
    bool playing = false;
    int players = 0;
    bool joinable = true;
    uint32_t id = 0;   // this host's, so that a LAN list shows it once
};
bool answer_probe(int socket, const uint8_t* data, size_t size, const sockaddr_storage& from, socklen_t from_len,
                  const Offer& offer);
// Answers PROBEs on the discovery port from now on (a thread of its own), with what `offer` says
// at the time.
void serve_discovery(Offer (*offer)());
// The answers stop (a session started in place has ended).
void stop_discovery();

}  // namespace conker::net::lobby
