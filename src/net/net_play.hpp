// Online play by input lockstep (docs/online-multiplayer-plan.md, M3). Every machine runs the
// whole game; each game frame's input record (src/runtime/frame_input.cpp) is exchanged over UDP,
// and a frame starts only once every port's input for it is in. Star topology: guests send their
// pad to the host, the host merges every port's input for a frame and sends the merged record to
// every guest, so the host's merged record is the one truth for each frame.
//
//     --net-host [port]         host a session on UDP <port> (37800 by default); the host is port 1
//     --net-join <address:port> join one; guests get ports 2 to 4 in join order
//     --net-players <n>         the host waits for this many players in all (2 by default)
//     --net-delay <frames>      the host's input delay (from the measured round trip by default)
//     --net-want-port <n>       join: ask for port n (2 to 4), as the lobby's START does
//     --net-token <hex>         the lobby's session token: a host with one lets in only guests with it,
//                               before the start and after (a dropped player's own machine comes back
//                               by its secret)
//     --net-map                 host: open the port on the router (net/net_portmap.hpp)
//     --net-name <name>         host: the name the lobby's LAN list shows
//     --net-lobby               not a session: open the ONLINE lobby at boot (net/net_lobby.hpp)
//
// The host waits for every guest before the game starts, and sends each its port, the input delay,
// the host's settings (the replay header's "# switch" lines, applied as a replay applies them) and
// the host's EEPROM, which every peer then plays on a copy (saves/net/), so nobody's own save is
// touched. The fixed step (CONKER_FIXED_STEP=1) and the intro skip are on for everyone.
//
// A machine's controller 1, pad and mouse, plays its port. Its input sampled at frame n is used at
// frame n + delay, so the network has delay frames to bring it to everyone; frames up to the delay
// have every player's port connected with nothing pressed. A frame whose input is not in yet
// waits for it (the screen holds; the window's title says who is waited for).
//
// Every peer hashes the game's memory at the start of every frame (from 0x80082B20 up to the frame
// buffers at 0x803B0000, minus the clock words state_hash.cpp leaves out) and sends the hashes to
// the host, whose hashes are the truth. A guest whose hash differs is out of sync, and is dropped as
// one not heard from for 8 s is, or one whose game has not moved on for 8 s while the others wait
// for it: the host picks the first frame it has not merged yet, and from it
// that port has nothing pressed in every merged record, which carry the drop, so every peer
// switches on the same frame and the others play on. The character stands still until the match
// ends; from then on the port reads as unplugged, so the join screen can offer a bot for it. The
// dropped guest's own game stops with "OUT OF SYNC" (or "DROPPED"); the host going away, or its game
// not moving on for 16 s, ends the session for everyone ("HOST LEFT"). Only the host pauses a match.
//
// Rejoining: the host keeps every merged record. A dropped guest's game starts again by itself
// (the same command line, asking for its port), and a guest that quit can run --net-join again;
// the host sends it the session as it was at the start and a snapshot of its game (about 1.2 MB
// compressed), which the guest puts in place before replaying the few frames since; it plays its
// port again about 7 s after joining. Without a snapshot (one refused, or CONKER_NET_NO_SNAPSHOT=1)
// it gets the records from frame 1 and replays them with the game's time running fast and no sound
// (about 4x here). Either way, within 90 frames of the host it plays its port again from a frame a
// little ahead. A player out of
// sync twice, or dropped four times, is turned away. Out of sync, both machines write their hash
// of every frame to conker-recomp-data/net/desync-*.txt.
//
// The bottom left corner of the picture shows the ping and delay, a stall's "WAITING FOR P2", a
// dropped player, or why this machine's game stopped (drawn by the renderer only, see
// overlay_display_list); the window's title shows the same. CONKER_NET_TEST_DESYNC=<frame> makes a
// peer change a byte of game memory at that frame, to see a drop; CONKER_NET_TEST_HANG=<frame>[:<s>]
// stops a peer's game for a while (30 s by default) with its network going on; both are cleared
// when a game starts again to rejoin, unless CONKER_NET_TEST_KEEP=1. CONKER_NET_NO_REJOIN=1 keeps a
// dropped guest stopped; CONKER_NET_TEST_GUEST_PAUSE=1 (every peer) lets a guest pause a match.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "runtime/frame_input.hpp"

namespace conker::net {

constexpr uint16_t kDefaultPort = 37800;

// Takes the --net-* options out of argv (the ROM path stays argv[1]). False, with a message, on a
// bad option.
bool parse_args(int* argc, char** argv);

// Whether this run is a net session (--net-host or --net-join).
bool active();

// Whether --net-lobby asked for the ONLINE lobby to open at boot (a normal game, not a session).
bool lobby_at_boot();

// The input delay given on the command line (--net-delay), or -1 for one from the round trip.
int delay_option();

// This build and system, as the handshake and the lobby compare them.
std::string build_text();
std::string os_text();
uint64_t rom_hash();  // the US ROM's, or CONKER_NET_TEST_ROM's

// Starts this game again (execv) with its command line, minus every --net option, plus net_args:
// how the lobby's START puts every machine into the session. Returns only if it could not.
void relaunch(const std::vector<std::string>& net_args);

// Connects the session before the game starts: the host waits for its guests, a guest for the host's
// session. Blocks; false if it could not (the message says why).
bool connect();

// The session's port held by this machine's controller 1 (0 to 3).
int local_port();

// The local controller for a game port: this machine's controller 1 for its own port, -1 for a
// port another machine holds. Outside a session, the port itself.
int local_controller(int port);

// In the game's entry point, before any of its code: the session's EEPROM becomes the save.
void use_session_save();

// The record the game reads before its first frame: the session's ports connected, nothing pressed.
frame_input::Record boot_record();

// Once a game frame on the game thread, at the start of func_1501905C: hashes the game's memory for
// the frame, sends the local input sampled now (record's port 0 and mouse 0, for frame + delay),
// waits until every port's input for the frame is in and fills record with it.
void frame(uint8_t* rdram, frame_input::Record* record);

// The host's settings as replay header lines, applied by the guest at its first frame.
const std::string& host_settings();

// Whether this machine is replaying the session so far to rejoin it (the game's time runs fast).
bool catching_up();

// A short status for the corner of the picture and the window title ("" for none).
std::string status_line();
std::string title_status();

// The renderer's wrapper for a frame's display list (RDRAM address, physical): calls the game's
// list, then draws the status line in the bottom left corner from scratch RDRAM above the game's 4 MiB.
// Returns the address of the list to run in its place, or the game's own when there is nothing to
// draw.
uint32_t overlay_display_list(uint8_t* rdram, uint32_t game_list);

// Online, the end of the game memory every machine must keep the same (physical; the frame buffers
// lie above it): the renderer writes nothing it drew below it. 0 when not online.
uint32_t game_memory_end();

}  // namespace conker::net
