// Online play by input lockstep; see net_play.hpp.
//
// The wire: UDP datagrams, each sealed on the link between a guest and the host (net_seal.hpp: the
// guest asks for the host's key first, and its HELLO comes in a FIRST). Inside, little-endian, the
// magic "CKNA", a type, the session id (0 in a HELLO) and the guest's secret: 64 random bits the
// guest picks and sends in its HELLO, then in every datagram either way between it and the host. A datagram whose secret is not its sender's
// is ignored, so a sender that forges a peer's address but never saw its traffic cannot pose as it;
// a dropped player's port can be taken back only with its secret. Inputs need no reliable channel: every INPUT and MERGED carries every
// frame the other side has not acknowledged yet (up to 16), and both sides send every 10 ms, so a
// lost datagram costs nothing but 10 ms. The handshake (the session blob in chunks, START) is
// resent until answered.
//
//     HELLO    guest -> host   build, OS, the port it asks for, the cookie (0 at first) and the
//                              lobby's session token (0 for none)
//     COOKIE   host -> guest   the cookie for the guest's address: nothing else is sent to an address,
//                              or kept for it, before a HELLO brings its cookie back (a forged sender
//                              never sees it), and the COOKIE is smaller than the HELLO
//     REJECT   host -> guest   why not (another build, the session is full or started)
//     CHUNK    host -> guest   a piece of the session blob: port, players, settings, EEPROM
//     READY    guest -> host   the whole blob is in
//     START    host -> guest   every guest is ready; the input delay
//     STARTED  guest -> host   START seen
//     INPUT    guest -> host   the guest's port's records from a frame on, its last hashes
//     MERGED   host -> guest   every port's records from a frame on (with the ports held and the
//                              ports dropped by then), and every drop: a port, its first frame and why
//     LEAVE    either way      this machine is quitting (sent a few times as it exits): the host drops
//                              the guest's port at once, and a guest learns the session is over
//
// The game socket also answers the lobby's PROBE (net/net_lobby.hpp), so that JOIN finds a
// running session.
#include "net/net_play.hpp"

#ifndef CONKER_NET_INTERNET_DEFAULT
#define CONKER_NET_INTERNET_DEFAULT 0  // set by CMake (CONKER_NET_INTERNET_DEFAULT)
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include "recomp.h"
#include "build_version.h"
#include "miniz.h"

#include "platform/audio.hpp"
#include "platform/display_mode.hpp"
#include "platform/process.hpp"
#include "runtime/game.hpp"
#include "net/net_codes.hpp"
#include "net/net_keys.hpp"
#include "net/net_lobby.hpp"
#include "net/net_portmap.hpp"
#include "net/net_seal.hpp"
#include "net/net_socket.hpp"
#include "runtime/snapshot.hpp"
#include "ultramodern/ultramodern.hpp"
#include "librecomp/mods.hpp"

namespace {

using conker::frame_input::Mouse;
using conker::frame_input::Port;
using conker::frame_input::Record;
using conker::net::socket_error_text;
using conker::net::socket_poll;
using conker::net::socket_poll_in;
using conker::net::socket_prepare;
using conker::net::socket_receive_from;
using conker::net::socket_send_to;
using conker::net::socket_set_option;
namespace process = conker::process;
using Clock = std::chrono::steady_clock;

inline gpr kseg0(uint32_t address) { return static_cast<gpr>(static_cast<int32_t>(address)); }

constexpr uint32_t kMagic = 0x414E4B43;     // "CKNA": inside sealed datagrams (net_seal.hpp)
constexpr uint32_t kCookiePurpose = 0x4E;   // address_cookie's purpose for a session's HELLO
constexpr int kPorts = conker::input::kPorts;
constexpr uint32_t kRing = 1024;          // frames of inputs, merged records and hashes kept
constexpr uint32_t kFramesPerPacket = 16;
constexpr uint8_t kTailChanged = 0x10;  // a MERGED record mask bit: its held/dropped/snapshot bytes follow
constexpr uint8_t kAfterBase = 0x20;    // the first record's mask bit: changes from frame first - 1, which
                                        // the receiver has (it acknowledged it), not the whole record
constexpr uint32_t kHashesPerPacket = 8;
constexpr int64_t kSendEveryMs = 10;
// The update packets on the tick (send_updates), which only resend what is not acknowledged and say
// the machine is there: new input goes at once. Every kSendEveryMs while a game waits for input, so
// that a lost packet costs little where it shows; otherwise this often. Each packet's headers were
// most of the traffic once its records went as changes.
constexpr int64_t kResendEveryMs = 100;
// The host never waits long for a guest. A guest whose input the host is waiting for and that has
// not been heard from for kQuietGraceMs (it quit without a LEAVE, was killed, or its network is
// down) is dropped: its port plays with nothing pressed and everyone else plays on. Its machine, if
// still there, learns of the drop from the next MERGED that reaches it, starts again and rejoins
// (relaunch_to_rejoin), so it catches up instead of going its own way. A guest sends every 10 ms
// whatever its game is doing, so silence means its machine or link, not a slow frame: in clean
// matches, at the auto delay and with 150 and 250 ms round trips (20-30 ms of jitter), a guest the
// host waited for was never quiet for more than 32 ms, and Wi-Fi hiccups are a few hundred ms.
// 750 ms is far above both, and below the one-second freeze players notice.
constexpr int64_t kQuietGraceMs = 750;
// A guest not heard from for this long is dropped even when the host is not waiting for it yet (its
// inputs were ahead); and the host not heard from for this long ends the session for a guest.
constexpr int64_t kDropAfterMs = 8000;
// A guest still heard from whose game has not moved on for this long while the host waits for it
// (its game hung) is dropped the same way. A slow frame is not that: loading a level, one machine's
// frame can take the other's up to about 0.8 s longer (two Macs on a LAN). A guest gives the host
// twice as long, so that the host has dropped a hung guest, and play has gone on, before a guest
// gives up on the host.
constexpr int64_t kStuckAfterMs = 3000;
constexpr int64_t kHostStuckAfterMs = 2 * kStuckAfterMs;
constexpr int64_t kStallShownMs = 1000;   // "waiting for" stays up this long after a stall
constexpr int64_t kStallCountsMs = 100;   // shorter waits are not shown
constexpr int kMaxDelay = 15;
// A rejoining guest replays the host's merged records from frame 1 with the game's time running
// this much faster, until it is within kLiveWithin frames of the host, which then has it play from
// kLiveLead frames on. The game itself manages about 4.7x: every frame waits for RT64 to draw it and
// put the depth back in RDRAM (the game reads it), a round trip to the GPU, and for its audio task;
// the clock only shortens the scheduler's waits between them (8x gave 4.1x, 16x 4.1x, 32x 4.7x, 64x
// and 128x no better). CONKER_NET_CATCH_UP_SPEED sets another, to measure.
constexpr uint32_t kCatchUpSpeed = 32;
constexpr uint32_t kCatchUpFrom = 60;   // further behind than this, a guest catches up
constexpr uint32_t kLiveWithin = 90;
constexpr uint32_t kLiveLead = 120;
// A frame number from the wire no session reaches (over a year at 60 a second), and far enough from
// 2^32 that frame + kRing and the like never wrap: a snapshot's, the host's last merged frame or a drop
// frame past it is refused. It was a day at 60 a second (48 hours at 30) until the hash history grew
// from the first frame played instead of from frame 0; after that a session's drops were ignored.
constexpr uint32_t kMaxFrame = 0x7FFFFFFF;
// A snapshot of a frame further ahead of the host's MERGED than this is refused (it is taken from a
// frame the host has merged, so a little behind its latest MERGED, never far ahead).
constexpr uint32_t kSnapFrameSlack = 30 * 60;
// Catching up that spends half of kCatchUpCheckMs waiting for the host's records is not catching up
// (a host that says it is further on than its records go, or one held up): it stops, and starts
// again at most twice more.
constexpr int64_t kCatchUpCheckMs = 4000;
constexpr int kMaxDesyncs = 2;          // a player out of sync this often cannot rejoin
constexpr int kMaxDrops = 4;            // nor one dropped this often for any reason (a loop of rejoins)
constexpr size_t kEepromSize = 0x800;     // 16 kbit
constexpr size_t kChunkSize = 1024;
constexpr size_t kMaxPacket = 1500;
// In place (the lobby's START), connecting gives up after this long: the players were all in the
// lobby a moment before.
constexpr int64_t kInPlaceConnectMs = 30000;

#ifdef __APPLE__
constexpr const char* kOs = "macos";
#elif defined(__linux__)
constexpr const char* kOs = "linux";
#elif defined(_WIN32)
constexpr const char* kOs = "windows";
#else
constexpr const char* kOs = "other";
#endif
#if defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
constexpr const char* kCpu = "arm64";
#else
constexpr const char* kCpu = "x86_64";
#endif

// The game thread waits for inputs on this queue (in scratch RDRAM above the game's 4 MiB, next to
// frame_input.cpp's), so the game's other threads run meanwhile; the network thread posts to it.
constexpr uint32_t kWaitQueue = 0x807FC100;
constexpr uint32_t kWaitSlots = 0x807FC120;
constexpr int32_t kWaitSlotCount = 8;

// The game memory hashed every frame (physical): above the audio buffers, below the frame buffers,
// without the words that hold clock readings (state_hash.cpp's masked words).
constexpr uint32_t kHashStart = 0x00082B20;
constexpr uint32_t kHashEnd = 0x003B0000;
struct Range {
    uint32_t start;
    uint32_t end;
};
constexpr Range kHashSkips[] = {
    {0x000BE6E8, 0x000BE6F8},  // the OSTimer the game sets at 0x800BE6E0: interval and value
    {0x000BEA18, 0x000BEA1C},  // the game thread's message queue D_800BEA10: its count
    {0x000BEA28, 0x000BEA68},  // and its slots
};

enum Type : uint8_t { kHello = 1, kReject, kChunk, kReady, kStart, kStarted, kInput, kMerged, kSnapWant, kSnapChunk, kSnapAck,
                     kCookie, kLeave };
// A guest not ready yet is let go after this long without a word, and a ready one, which says READY
// again every kReadyAgainMs while it waits for the others, after kDropAfterMs.
constexpr int64_t kUnreadyGoneMs = 5000;
constexpr int64_t kReadyAgainMs = 500;

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

// --- options ---------------------------------------------------------------------------------------

struct Options {
    bool host = false;
    bool join = false;
    uint16_t port = conker::net::kDefaultPort;
    std::string address;  // join: host:port
    int players = 2;
    int delay = -1;       // -1: from the round trip
    int want_port = 0;    // join: the port the lobby gave this machine (1 to 4), 0 for any
    bool map = false;     // host: open the port on the router (the lobby's HOST did)
    bool lobby = false;   // open the ONLINE lobby at boot (not a session)
    std::string name = "Player";  // the name the LAN list shows for this host
    uint64_t token = 0;   // the session token (--net-token, the lobby's or a host's own): the host lets in only guests with it
    bool open = false;    // --net-open: a host without a token, which lets in anyone with the same build
    // --net-unsafe-internet: not the LAN only (net_codes.hpp). On from the start in a build made with
    // CONKER_NET_INTERNET_DEFAULT (test builds, for now), where --net-lan-only turns it off.
    bool internet = CONKER_NET_INTERNET_DEFAULT != 0;
} g_options;

std::atomic<bool> g_active{false};  // a session is on (the game thread reads it every frame)
uint64_t g_secret = 0;  // a guest: its secret (see the wire, above)
namespace seal = conker::net::seal;
seal::KeyPair g_host_key;   // the host's: this machine's host key (net_keys.hpp), the same as its lobby's
seal::KeyPair g_guest_key;  // a guest's, new each run
seal::Expected g_expected;  // a guest: the host's key, from the lobby (CONKER_NET_HOST_PUBLIC) or --net-host-key
seal::Channel g_contact;    // the host, under g_mutex: the link of the HELLO being handled (its FIRST's)
// The host: after this long dropped, a player's port may go to a newcomer (a machine with another
// secret) when no port is free. Until then only the player's own machine can have it back.
constexpr int64_t kReclaimAfterMs = 60000;

// --- wire ------------------------------------------------------------------------------------------

struct Writer {
    uint8_t data[8192];  // a packet, or the session blob
    size_t size = 0;
    void u8(uint8_t v) { if (size < sizeof(data)) data[size++] = v; }
    void u16(uint16_t v) { u8(v & 0xFF); u8(v >> 8); }
    void u32(uint32_t v) { u16(v & 0xFFFF); u16(v >> 16); }
    void u64(uint64_t v) { u32(static_cast<uint32_t>(v)); u32(static_cast<uint32_t>(v >> 32)); }
    void f32(float v) { uint32_t bits; std::memcpy(&bits, &v, 4); u32(bits); }
    void bytes(const void* p, size_t n) { for (size_t i = 0; i < n; i++) u8(static_cast<const uint8_t*>(p)[i]); }
    void text(const std::string& s, size_t max) {
        const size_t n = std::min(s.size(), max);
        u16(static_cast<uint16_t>(n));
        bytes(s.data(), n);
    }
};

struct Reader {
    const uint8_t* data;
    size_t size;
    size_t at = 0;
    bool ok = true;
    uint8_t u8() {
        if (at >= size) { ok = false; return 0; }
        return data[at++];
    }
    uint16_t u16() { const uint16_t lo = u8(); return static_cast<uint16_t>(lo | (u8() << 8)); }
    uint32_t u32() { const uint32_t lo = u16(); return lo | (static_cast<uint32_t>(u16()) << 16); }
    uint64_t u64() { const uint64_t lo = u32(); return lo | (static_cast<uint64_t>(u32()) << 32); }
    float f32() { const uint32_t bits = u32(); float v; std::memcpy(&v, &bits, 4); return std::isfinite(v) ? v : 0.0f; }
    std::string text(size_t max) {
        const size_t n = u16();
        if (n > max || at + n > size) { ok = false; return {}; }
        std::string s(reinterpret_cast<const char*>(data + at), n);
        at += n;
        return s;
    }
};

// Whether a chunk of a blob of `total` bytes (at most `max`) at `offset` is one of its chunks: on a
// chunk boundary, inside the blob, and as long as a chunk there is.
bool chunk_fits(uint32_t total, uint32_t max, uint32_t offset, uint32_t length, uint32_t chunk) {
    return total != 0 && total <= max && offset < total && offset % chunk == 0 &&
           length == std::min<uint64_t>(chunk, uint64_t{total} - offset);
}

void write_port(Writer& w, const Port& p, const Mouse& m) {
    w.u16(p.buttons);
    w.u8(static_cast<uint8_t>(p.stick_x));
    w.u8(static_cast<uint8_t>(p.stick_y));
    w.u8(static_cast<uint8_t>(p.right_x));
    w.u8(static_cast<uint8_t>(p.right_y));
    w.u8(p.flags);
    w.u8(m.captured ? 1 : 0);
    w.u8(static_cast<uint8_t>(m.zoom));
    w.f32(m.dx);
    w.f32(m.dy);
}

// A port's bytes as write_port writes them, to tell whether it changed from one frame to the next.
constexpr size_t kPortBytes = 17;
std::array<uint8_t, kPortBytes> port_bytes(const Port& p, const Mouse& m) {
    Writer w;
    write_port(w, p, m);
    std::array<uint8_t, kPortBytes> bytes{};
    std::memcpy(bytes.data(), w.data, kPortBytes);
    return bytes;
}

// A frame's mouse as every machine plays it: a real mouse moves at most a few hundred pixels a frame,
// and a peer's huge (finite) motion would turn a MULTI player's facing to infinity on every machine.
constexpr float kMouseLimit = 4096.0f;
Mouse limited(Mouse m) {
    m.dx = std::clamp(m.dx, -kMouseLimit, kMouseLimit);
    m.dy = std::clamp(m.dy, -kMouseLimit, kMouseLimit);
    m.zoom = std::clamp<int8_t>(m.zoom, -2, 2);
    return m;
}

void read_port(Reader& r, Port* p, Mouse* m) {
    p->buttons = r.u16();
    p->stick_x = static_cast<int8_t>(r.u8());
    p->stick_y = static_cast<int8_t>(r.u8());
    p->right_x = static_cast<int8_t>(r.u8());
    p->right_y = static_cast<int8_t>(r.u8());
    p->flags = r.u8();
    m->captured = r.u8() != 0;
    m->zoom = static_cast<int8_t>(r.u8());
    m->dx = r.f32();
    m->dy = r.f32();
    *m = limited(*m);
}

// --- session state (under g_mutex unless noted) -----------------------------------------------------

std::mutex g_mutex;
int g_socket = -1;           // set before the network thread starts
uint32_t g_session = 0;
int g_local_port = 0;
uint8_t g_ports = 0;         // the session's ports, a bit each (the host adds a newcomer's)
uint8_t g_start_ports = 0;   // the ports held when the session started
int g_delay = 2;
std::string g_settings;      // the host's replay header lines
bool g_wide = false;         // the host's widescreen: the game's culling works to 16:9 for everyone
std::vector<uint8_t> g_eeprom;

struct Input {
    uint32_t frame = 0;
    Port port;
    Mouse mouse;
};
Input g_inputs[kPorts][kRing];  // the host: every port's; a guest: its own
uint32_t g_input_upto[kPorts];  // the last frame of each port's inputs in, with none missing

struct Merged {
    uint32_t frame = 0;
    Port ports[kPorts];
    Mouse mice[kPorts];
    uint8_t held = 0;     // the ports held by players at this frame, a bit each (a newcomer adds one)
    uint8_t dropped = 0;  // the ports the host has dropped by this frame, a bit each
    uint8_t snapshot = 0;  // 1: the host takes a snapshot at this frame's start, and every peer's
                           // renderer forgets its frame buffers (see snapshot::request_framebuffer_discard)
};
Merged g_merged[kRing];
std::vector<Merged> g_history;  // the host: every merged record from frame delay + 1, for a rejoin
uint32_t g_host_upto = 0;       // a guest: the host's last merged frame, from its MERGED
uint32_t g_merged_upto = 0;     // the last frame merged (host) or received (guest), with none missing

struct Hash {
    uint32_t frame = 0;
    uint64_t value = 0;
};
Hash g_own_hashes[kRing];
uint32_t g_own_hash_upto = 0;
std::vector<uint64_t> g_own_hash_history;  // every frame's own hash from g_own_hash_base on (0: not played),
                                           // for rejoins and desync logs
uint32_t g_own_hash_base = 0;              // the frame of g_own_hash_history[0]: the first played here

// Under g_mutex: this machine's hash of frame, or nullptr if it has not played it.
const uint64_t* own_hash(uint32_t frame) {
    if (frame < g_own_hash_base || frame - g_own_hash_base >= g_own_hash_history.size() ||
        g_own_hash_history[frame - g_own_hash_base] == 0) {
        return nullptr;
    }
    return &g_own_hash_history[frame - g_own_hash_base];
}
Hash g_peer_hashes[kPorts][kRing];  // by the peer's port
uint32_t g_peer_played[kPorts] = {};  // the host: the last frame each guest has hashed, so played

struct Peer {
    sockaddr_storage addr{};
    socklen_t addr_len = 0;
    int port = 0;
    uint64_t secret = 0;       // the guest's secret (see the wire, above)
    conker::net::seal::Channel channel;  // the sealed link with this peer (net_seal.hpp)
    int64_t last_heard = 0;
    int64_t dropped_at = 0;    // the host: when it was dropped (a long-dropped port may go to a newcomer)
    int64_t moved_on = 0;      // when the peer's game last moved on (the host: its input; a guest: the
                               // merged records), 0 before its first frame
    uint32_t blocking_frame = 0;  // the host: the frame it has waited for this guest's input of, if any,
    int64_t blocking_since = 0;   // and since when
    uint32_t acked = 0;        // the host: the merged frames this guest has; a guest: its inputs the host has
    uint32_t echo = 0;         // the peer's last send time, to echo
    int64_t echo_at = 0;       // when it came
    int rtt = -1;
    bool ready = false;        // the host: the guest has the whole blob
    bool started = false;      // the host: the guest saw START
    bool dropped = false;      // the host: the guest is dropped (g_drop_frame)
    bool rejoining = false;    // the host: a dropped guest's machine is back, replaying to catch up
    std::vector<uint8_t> blob; // the host: the session sent to a rejoining guest
    // The host: a rejoining guest's snapshot of the game, and which of its chunks it has.
    uint64_t snap_wanted = 0;  // the snapshot it waits for (g_snap_serial), 0 for none
    std::shared_ptr<const std::vector<uint8_t>> snap;
    uint32_t snap_frame = 0;
    std::vector<bool> snap_acked;
    size_t snap_next = 0;
    int64_t snap_since = 0;
    bool used_snapshot = false;  // rejoined from one, not yet playing again
    // The host: a guest of a session started in place (the lobby's START), or one rejoining in place,
    // whose game becomes the host's only from a snapshot: it plays its port once it has played past
    // the snapshot's frame (until then its hashes are of its own game, and it sends none).
    bool needs_snapshot = false;
};
std::vector<Peer> g_peers;     // the host: its guests; a guest: the host

// Dropping a player: the host decides the first frame the port plays with nothing pressed, the
// first frame it has not merged yet, so that no peer has run it, and every merged record from then
// on carries it (Merged::dropped). The others play on. The host's hashes are the truth: a guest
// whose hash differs is dropped as one who left, and its own game stops.
// kNew: a newcomer, not yet playing; kBack: a dropped player's machine is back and catching up.
enum Drop : uint8_t { kNotDropped = 0, kLeft = 1, kOutOfSync = 2, kNew = 3, kBack = 4 };
uint32_t g_drop_frame[kPorts] = {};  // 0, or the first frame the port is dropped from
uint8_t g_drop_reason[kPorts] = {};
// A rejoined player plays again from its live frame (after its drop frame); a later drop moves the
// drop frame past it and clears it.
uint32_t g_live_frame[kPorts] = {};
int g_desyncs[kPorts] = {};          // the host: how often each port's player was out of sync
int g_drops[kPorts] = {};            // the host: how often each port's player was dropped
uint32_t g_rejoin_from = 0;          // a rejoining guest: its port's drop frame when it came back

// Rejoining from a snapshot of the host's game (runtime/snapshot.hpp) instead of replaying the
// session from frame 1: the guest boots, asks for one (SNAPWANT) once its game is at its first frame
// that needs the host's records, and the host marks the next frame it merges for one (every peer's
// renderer forgets its frame buffers at that frame, so all of them draw it from RDRAM alike), takes
// it at that frame's start, compresses it and sends it in chunks the guest acknowledges (SNAPCHUNK,
// SNAPACK). The guest puts it in place
// and goes on from that frame, replaying the few frames since. If it cannot (its threads elsewhere,
// say) it replays from frame 1 as before; if its game then differs from the host's, the host has it
// start again and replay (the port's snapshots are off from then on).
constexpr size_t kSnapChunkSize = 1024;
constexpr int kSnapChunksPerTick = 32;
constexpr int64_t kSnapWaitMs = 30000;
bool g_snap_failed[kPorts] = {};              // the host: a snapshot did not work for this port
std::atomic<bool> g_snap_requested{false};    // the host: mark the next frame merged for one
std::shared_ptr<const std::vector<uint8_t>> g_snap_blob;  // the host, under g_mutex: the newest, compressed
uint32_t g_snap_frame = 0;                    // the host, under g_mutex: its frame
uint64_t g_snap_serial = 0;                   // the host, under g_mutex: snapshots made so far
bool g_snap_coming = false;                   // a guest: the host sends it one
bool g_snap_tried = false;                    // a guest, game thread: waited for it and tried it
std::vector<uint8_t> g_snap_data;             // a guest, under g_mutex: as it comes
std::vector<bool> g_snap_have;
size_t g_snap_have_count = 0;
uint32_t g_snap_of = 0;                       // its frame
std::atomic<bool> g_snap_waiting{false};      // a guest's game waits for it
int64_t g_join_start = 0;                     // a guest: when it began joining
bool g_live_announced = false;                // a rejoining guest: said when it plays again
std::atomic<bool> g_catching_up{false};
// A guest catching up (frame(), game thread): when it began, from which frame, the current check of
// whether it is catching up at all, whether it has caught up (once only: a guest catches up after
// joining, then keeps level) and the catch-ups that were not.
struct CatchUp {
    int64_t start = 0;
    uint32_t first = 0;
    int64_t check_start = 0;
    int64_t check_waited = 0;
    bool caught_up = false;
    int stalled = 0;
} g_catch_up;
// How long frames waited for input, printed every 900 frames (30 s) for testing links: the waits of
// 50 ms or more (stalls a player sees as a hitch), their total and the longest, the round trip.
struct WaitStats {
    uint32_t stalls = 0;
    int64_t stalled_ms = 0;
    int64_t longest = 0;
    int64_t waited_ms = 0;
    uint32_t frames = 0;
    int64_t started = 0;
} g_wait_stats;  // game thread
std::vector<std::string> g_argv;     // the command line, for relaunching to rejoin
std::vector<std::string> g_plain_argv;  // the command line without the --net options, for the lobby

// Sessions started in place: the lobby's START no longer starts every game again. The session is
// connected on a thread of its own while the game runs (kConnecting); then, at the start of a frame,
// the game thread turns the fixed step on (kConnected to kEntering: frame_input's enter_fixed_step,
// which takes the audio frames from Rare's audio thread), keeps a snapshot of this machine's own
// game, moves to the session's save copy and the host's settings and counts frames from 0 (kInSession).
// The host's game goes on from where it is (the title, on the MULTI door); each guest's port is held
// with nothing pressed until its game has become the host's from a snapshot (the rejoin path: its
// own game until then is never compared). Leaving puts the snapshot of this machine's own game back,
// with its save and settings, and turns the fixed step off: the player is where they were before the
// session. A guest dropped from the session connects again the same way (no restart).
enum Place : int { kNoPlace = 0, kConnecting, kConnected, kEntering, kInSession, kLeaving };
std::atomic<int> g_place{kNoPlace};
bool g_in_place = false;              // this session was started in place
std::thread g_connect_thread;
std::atomic<bool> g_net_stop{false};  // the network and connect threads stop
std::vector<uint8_t> g_return;        // game thread: this machine's own game, put back on leaving
std::string g_own_settings;           // game thread: this machine's settings before the session
bool g_own_kept = false;              // game thread: g_return and g_own_settings are this machine's
std::atomic<bool> g_leave_requested{false};
std::atomic<bool> g_rejoin_requested{false};
int64_t g_stop_seen = 0;              // game thread: when it saw the session here stop
int g_rejoin_port = 0;                // an in-place rejoin: the port asked for (1 to 4)
uint64_t g_rejoin_secret_kept = 0;    // and this machine's secret, as before
int g_rejoin_count = 0;               // in-place rejoins in a row
std::mutex g_place_mutex;
std::string g_place_message;          // under g_place_mutex: why connecting failed or the session ended
void set_place_message(const std::string& text) {
    std::lock_guard lock(g_place_mutex);
    g_place_message = text;
}

// Whether a port plays with nothing pressed at frame.
bool dropped_at(int port, uint32_t frame) {
    const uint32_t from = g_drop_frame[port];
    const uint32_t live = g_live_frame[port];
    return from != 0 && frame >= from && !(live > from && frame >= live);
}

// This machine's own game has stopped (nothing pressed on any port, no more waiting): dropped by
// the host as out of sync, or the host is gone, which ends the session for everyone.
enum Stop : int { kPlaying = 0, kStoppedOutOfSync = 1, kHostLeft = 2, kStoppedDropped = 3 };
std::atomic<int> g_stopped{kPlaying};
constexpr uint32_t kMatchFlag = 0x800BE616;  // D_800BE616: nonzero during a match
// The pause (func_151DDC20, the in-play screen's handler): Start's edge on any pad opens it, and only
// that pad (D_800E0A98) works it and closes it. Online only the host's port pauses a match.
constexpr uint32_t kScreenHandler = 0x800E0B94;  // D_800E0B94: the frame's screen handler, 0 in play
constexpr uint32_t kPauseOpen = 0x800BEAC1;      // D_800BEAC1: set when Start opens the pause
constexpr uint32_t kPauseRunning = 0x800BEAC0;   // D_800BEAC0: its copy while the pause runs
constexpr uint32_t kPausePad = 0x800E0A98;       // D_800E0A98: the pad that paused
constexpr uint16_t kStartButton = 0x1000;
bool g_unplugged[kPorts] = {};       // game thread: a dropped port now reads as unplugged
const bool g_snapshot_fields = (conker::snapshot::add_bools("net_play.unplugged", g_unplugged, kPorts), true);
uint32_t g_stopped_frame = 0;        // under g_mutex: the frame it stopped from
uint8_t g_host_waiting_for = 0;   // a guest: the ports the host's last MERGED said it waited for
std::atomic<bool> g_waiting{false};
std::atomic<bool> g_queue_made{false};
std::atomic<int64_t> g_stall_seen{0};  // when a stall longer than kStallCountsMs was last seen
std::atomic<int> g_stall_port{-1};
std::thread g_thread;

bool is_host() { return g_options.host; }

// The round trip from a READY's echo of the host's CHUNK time, held to what a link can take: the echo
// is the guest's word, and the largest round trip sets everyone's input delay.
int rtt_from_echo(uint32_t sent) {
    const int64_t rtt = static_cast<int64_t>(static_cast<uint32_t>(now_ms()) - sent);
    return static_cast<int>(std::clamp<int64_t>(rtt, 0, 1000));
}

Port neutral_port() {
    Port port;
    port.flags = Port::kConnected | Port::kRumble;
    return port;
}

// What a held port reads: connected, with a rumble pak (always, so that the game, which looks at
// the paks at boot, sees the same as later), the right stick, INVERT AIM, CONSISTENT CONTROLS and the
// camera swap as the sender has them.
Port normalized(Port port) {
    port.flags = Port::kConnected | Port::kRumble |
                 (port.flags & (Port::kRightStick | Port::kInvertAim | Port::kConsistentControls | Port::kCameraSwap));
    return port;
}

// --- sockets ---------------------------------------------------------------------------------------

bool same_address(const sockaddr_storage& a, socklen_t a_len, const sockaddr_storage& b, socklen_t b_len) {
    if (a.ss_family != b.ss_family) {
        return false;
    }
    if (a.ss_family == AF_INET) {
        const auto& x = reinterpret_cast<const sockaddr_in&>(a);
        const auto& y = reinterpret_cast<const sockaddr_in&>(b);
        return x.sin_port == y.sin_port && x.sin_addr.s_addr == y.sin_addr.s_addr;
    }
    if (a.ss_family == AF_INET6) {
        const auto& x = reinterpret_cast<const sockaddr_in6&>(a);
        const auto& y = reinterpret_cast<const sockaddr_in6&>(b);
        return x.sin6_port == y.sin6_port && std::memcmp(&x.sin6_addr, &y.sin6_addr, sizeof(x.sin6_addr)) == 0;
    }
    return a_len == b_len && std::memcmp(&a, &b, a_len) == 0;
}

std::string address_text(const sockaddr_storage& addr, socklen_t len) {
    char host[NI_MAXHOST] = "?";
    char service[NI_MAXSERV] = "?";
    getnameinfo(reinterpret_cast<const sockaddr*>(&addr), len, host, sizeof(host), service, sizeof(service),
                NI_NUMERICHOST | NI_NUMERICSERV);
    return std::string(host) + ":" + service;
}

// --- the network simulator ------------------------------------------------------------------------
//
// CONKER_NET_SIM=latency=<ms>,jitter=<ms>,loss=<%>,dup=<%>,reorder=<%>,outage=<from>-<to> makes this
// peer's link worse, for testing: every datagram it sends and receives waits half the round trip
// `latency` plus up to `jitter` (so the one peer with it has that round trip to everyone), `loss`
// of those it sends are lost, `dup` sent twice and `reorder` held back 40 ms more, past the next
// ones; from `from` to `to` seconds after the session starts nothing gets through either way.

struct Sim {
    bool on = false;
    int latency = 0;   // round trip, ms
    int jitter = 0;    // ms, one way, up to
    double loss = 0;   // percent
    double dup = 0;
    double reorder = 0;
    int64_t outage_from = -1;  // s after the session starts
    int64_t outage_to = -1;
};

const Sim g_sim = [] {
    Sim sim;
    const char* value = std::getenv("CONKER_NET_SIM");
    if (value == nullptr || value[0] == '\0') {
        return sim;
    }
    sim.on = true;
    std::string text = value;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find(',', at);
        if (end == std::string::npos) {
            end = text.size();
        }
        const std::string item = text.substr(at, end - at);
        const size_t equals = item.find('=');
        const std::string key = item.substr(0, equals);
        const std::string number = equals == std::string::npos ? "" : item.substr(equals + 1);
        if (key == "latency") sim.latency = std::atoi(number.c_str());
        else if (key == "jitter") sim.jitter = std::atoi(number.c_str());
        else if (key == "loss") sim.loss = std::atof(number.c_str());
        else if (key == "dup") sim.dup = std::atof(number.c_str());
        else if (key == "reorder") sim.reorder = std::atof(number.c_str());
        else if (key == "outage") std::sscanf(number.c_str(), "%lld-%lld", reinterpret_cast<long long*>(&sim.outage_from),
                                              reinterpret_cast<long long*>(&sim.outage_to));
        else std::fprintf(stderr, "[conker] CONKER_NET_SIM: unknown %s\n", key.c_str());
        at = end + 1;
    }
    std::printf("[conker] net: simulating a link: %d ms round trip, jitter %d ms, loss %.1f%%, dup %.1f%%, reorder "
                "%.1f%%, outage %lld-%lld s\n", sim.latency, sim.jitter, sim.loss, sim.dup, sim.reorder,
                static_cast<long long>(sim.outage_from), static_cast<long long>(sim.outage_to));
    return sim;
}();

struct Delayed {
    int64_t due;
    sockaddr_storage addr;
    socklen_t addr_len;
    std::vector<uint8_t> data;
};
std::mutex g_sim_mutex;
std::vector<Delayed> g_sim_out;  // under g_sim_mutex
std::vector<Delayed> g_sim_in;   // the receiving thread's
std::mt19937 g_sim_random{12345};  // under g_sim_mutex
std::atomic<int64_t> g_session_start{0};

bool sim_chance(double percent) { return percent > 0 && std::uniform_real_distribution<double>(0, 100)(g_sim_random) < percent; }

int64_t sim_delay() {
    int64_t delay = g_sim.latency / 2;
    if (g_sim.jitter > 0) {
        delay += std::uniform_int_distribution<int>(0, g_sim.jitter)(g_sim_random);
    }
    return delay;
}

bool sim_outage(int64_t now) {
    const int64_t start = g_session_start.load();
    if (start == 0 || g_sim.outage_from < 0) {
        return false;
    }
    const int64_t seconds = (now - start) / 1000;
    return seconds >= g_sim.outage_from && seconds < g_sim.outage_to;
}

void send_datagram(const sockaddr_storage& addr, socklen_t len, const uint8_t* data, size_t size) {
    if (!g_sim.on) {
        socket_send_to(g_socket, data, size, reinterpret_cast<const sockaddr*>(&addr), len);
        return;
    }
    const int64_t now = now_ms();
    std::lock_guard lock(g_sim_mutex);
    if (sim_outage(now) || sim_chance(g_sim.loss)) {
        return;
    }
    const int copies = sim_chance(g_sim.dup) ? 2 : 1;
    for (int i = 0; i < copies; i++) {
        int64_t due = now + sim_delay();
        if (sim_chance(g_sim.reorder)) {
            due += 40;
        }
        g_sim_out.push_back({due, addr, len, std::vector<uint8_t>(data, data + size)});
    }
}

// Sends what is due; the time until the next one is due (or -1).
int64_t sim_pump() {
    if (!g_sim.on) {
        return -1;
    }
    const int64_t now = now_ms();
    std::lock_guard lock(g_sim_mutex);
    int64_t next = -1;
    for (size_t i = 0; i < g_sim_out.size();) {
        Delayed& d = g_sim_out[i];
        if (d.due <= now) {
            socket_send_to(g_socket, d.data.data(), d.data.size(), reinterpret_cast<const sockaddr*>(&d.addr), d.addr_len);
            g_sim_out[i] = std::move(g_sim_out.back());
            g_sim_out.pop_back();
            continue;
        }
        next = next < 0 ? d.due - now : std::min(next, d.due - now);
        i++;
    }
    return next;
}

// A packet sealed on a link and sent.
void send_sealed(seal::Channel& channel, const sockaddr_storage& addr, socklen_t len, const Writer& w) {
    uint8_t out[sizeof(w.data) + seal::kOverhead];
    const size_t size = seal::seal(channel, w.data, w.size, out, sizeof(out));
    if (size > 0) {
        send_datagram(addr, len, out, size);
    }
}

void send_to(Peer& peer, const Writer& w) { send_sealed(peer.channel, peer.addr, peer.addr_len, w); }

// The host: an answer to the HELLO being handled, on its FIRST's link.
void send_contact(const sockaddr_storage& addr, socklen_t len, const Writer& w) { send_sealed(g_contact, addr, len, w); }

// The host's key and a cookie for the sender, the answer to an ASK (or to a FIRST whose cookie is too
// old): smaller than either.
void send_key_offer(const sockaddr_storage& from, socklen_t from_len) {
    uint8_t out[64];
    const size_t size = seal::write_offer(g_host_key, conker::net::address_cookie(from, kCookiePurpose), out, sizeof(out));
    send_datagram(from, from_len, out, size);
}

void begin(Writer& w, Type type, uint64_t secret) {
    w.u32(kMagic);
    w.u8(type);
    w.u32(g_session);
    w.u64(secret);
}

// The time fields every INPUT and MERGED carries: the send time, and the peer's last send time with
// how long ago it came, for the round trip.
void write_times(Writer& w, const Peer& peer) {
    const int64_t now = now_ms();
    w.u32(static_cast<uint32_t>(now));
    w.u32(peer.echo);
    w.u16(static_cast<uint16_t>(std::min<int64_t>(peer.echo_at != 0 ? now - peer.echo_at : 0, 0xFFFF)));
}

void read_times(Reader& r, Peer& peer) {
    const uint32_t sent = r.u32();
    const uint32_t echo = r.u32();
    const uint16_t held = r.u16();
    if (!r.ok) {
        return;
    }
    const int64_t now = now_ms();
    peer.echo = sent;
    peer.echo_at = now;
    if (echo != 0) {
        const int64_t rtt = static_cast<int64_t>(static_cast<uint32_t>(now) - echo) - held;
        if (rtt >= 0 && rtt < 10000) {
            peer.rtt = peer.rtt < 0 ? static_cast<int>(rtt) : (peer.rtt * 7 + static_cast<int>(rtt)) / 8;
        }
    }
}

// --- hashes ----------------------------------------------------------------------------------------

uint64_t hash_words(const uint8_t* rdram, uint32_t start, uint32_t end, uint64_t hash) {
    uint32_t at = start;
    for (; at + 8 <= end; at += 8) {
        uint64_t word;
        std::memcpy(&word, rdram + at, 8);
        hash = (hash ^ word) * 0xFF51AFD7ED558CCDull;
        hash ^= hash >> 32;
    }
    for (; at + 4 <= end; at += 4) {
        uint32_t word;
        std::memcpy(&word, rdram + at, 4);
        hash = (hash ^ word) * 0xFF51AFD7ED558CCDull;
        hash ^= hash >> 32;
    }
    return hash;
}

// The hashed memory in 64 KiB blocks, one hash each, kept for the last kDetailFrames frames: a desync
// file carries them, so that two machines' files show where their games differ, not only when.
constexpr uint32_t kDetailBlock = 0x10000;
constexpr uint32_t kDetailFirst = kHashStart & ~(kDetailBlock - 1);
constexpr uint32_t kDetailBlocks = (kHashEnd - kDetailFirst + kDetailBlock - 1) / kDetailBlock;
constexpr uint32_t kDetailFrames = 128;
struct Detail {
    uint32_t frame = 0;
    uint32_t blocks[kDetailBlocks] = {};
};
Detail g_details[kDetailFrames];  // under g_mutex

void hash_blocks(const uint8_t* rdram, uint32_t* out) {
    for (uint32_t b = 0; b < kDetailBlocks; b++) {
        const uint32_t start = std::max(kDetailFirst + b * kDetailBlock, kHashStart);
        const uint32_t end = std::min(kDetailFirst + (b + 1) * kDetailBlock, kHashEnd);
        uint64_t hash = 0x9E3779B97F4A7C15ull;
        uint32_t at = start;
        for (const Range& skip : kHashSkips) {
            if (skip.end <= at || skip.start >= end) {
                continue;
            }
            hash = hash_words(rdram, at, skip.start, hash);
            at = skip.end;
        }
        hash = hash_words(rdram, at, end, hash);
        out[b] = static_cast<uint32_t>(hash ^ (hash >> 32));
    }
}

// Online the renderer writes nothing it drew into game memory (game_memory_end), the depth buffer
// included, since each machine's GPU makes slightly different depth and the game reads it back for
// the camera (func_1512B1B8). Each frame it reads a cleared buffer instead, as just after the
// game's own clear (0xFFFC, nothing near): the same on every machine.
constexpr uint32_t kDepthBuffer = 0x000BE9C4;  // D_800BE9C4: the depth buffer
constexpr uint32_t kScreenWidth = 0x000BE620;  // D_800BE620
constexpr uint32_t kScreenHeight = 0x000BE624; // D_800BE624
void clear_depth(uint8_t* rdram) {
    uint32_t depth;
    uint32_t width;
    uint32_t height;
    std::memcpy(&depth, rdram + kDepthBuffer, 4);
    std::memcpy(&width, rdram + kScreenWidth, 4);
    std::memcpy(&height, rdram + kScreenHeight, 4);
    depth &= 0x1FFFFFFF;
    if (width == 0 || height == 0 || width > 640 || height > 480 || depth < kHashStart ||
        depth + width * height * 2 > kHashEnd) {
        return;
    }
    const uint16_t clear = 0xFFFC;
    for (uint32_t at = depth; at + 2 <= depth + width * height * 2; at += 2) {
        std::memcpy(rdram + at, &clear, 2);  // every halfword the same, so its place in the word does not matter
    }
}

uint64_t hash_game(const uint8_t* rdram) {
    uint64_t hash = 0x9E3779B97F4A7C15ull;
    uint32_t at = kHashStart;
    for (const Range& skip : kHashSkips) {
        hash = hash_words(rdram, at, skip.start, hash);
        at = skip.end;
    }
    return hash_words(rdram, at, kHashEnd, hash);
}

void wake_game_thread();
bool merge();
void update_offer();
conker::net::lobby::Offer session_offer();

// The host, under g_mutex: from the first frame not merged yet, the guest's port has nothing pressed.
void write_hash_log(const char* role, int port, uint32_t frame);

void drop(Peer& peer, Drop reason, const char* why) {
    if (reason == kOutOfSync && peer.used_snapshot && peer.rejoining) {
        // Its game went its own way after the snapshot: no more snapshots for this port, and this one
        // does not count against the player; its game starts again and replays.
        g_snap_failed[peer.port] = true;
        std::fprintf(stderr, "[conker] net: player %d's game differs from the host's after the snapshot of frame %u: "
                             "it starts again and replays from frame 1\n", peer.port + 1, peer.snap_frame);
        write_hash_log("host", peer.port, g_merged_upto + 1);
    } else if (reason == kOutOfSync) {
        g_desyncs[peer.port]++;
        write_hash_log("host", peer.port, g_merged_upto + 1);
    }
    if (peer.dropped && !peer.rejoining) {
        return;
    }
    g_drops[peer.port]++;
    if (peer.rejoining) {
        // Caught out while catching up: the drop frame moves on, so that its game stops.
        peer.rejoining = false;
    }
    peer.dropped = true;
    peer.dropped_at = now_ms();
    g_drop_frame[peer.port] = g_merged_upto + 1;
    g_live_frame[peer.port] = 0;
    g_drop_reason[peer.port] = reason;
    std::fprintf(stderr, "[conker] net: player %d (%s) dropped from frame %u: %s; the others play on and player %d "
                         "has nothing pressed\n",
                 peer.port + 1, address_text(peer.addr, peer.addr_len).c_str(), g_drop_frame[peer.port], why,
                 peer.port + 1);
    merge();
    wake_game_thread();
    update_offer();
}

// The host, under g_mutex: compares frame's hashes where both are in. A guest that differs from
// the host is out of sync.
void compare_hashes(uint32_t frame) {
    const uint64_t* own_value = own_hash(frame);
    if (own_value == nullptr || frame > g_own_hash_upto) {
        return;
    }
    const uint64_t own = *own_value;
    for (Peer& peer : g_peers) {
        const Hash& theirs = g_peer_hashes[peer.port][frame % kRing];
        if ((!peer.dropped || peer.rejoining) && theirs.frame == frame && theirs.value != own) {
            std::fprintf(stderr, "[conker] net: player %d is OUT OF SYNC at frame %u: their game memory hashes to "
                                 "%016llx, the host's to %016llx\n",
                         peer.port + 1, frame, static_cast<unsigned long long>(theirs.value),
                         static_cast<unsigned long long>(own));
            drop(peer, kOutOfSync, "out of sync");
        }
    }
}

// Each frame's hash goes in the packets of the next kHashRepeats frames played, not in every packet
// for 8 frames: a lost packet loses no hash, and most packets carry one or two.
constexpr uint32_t kHashRepeats = 3;
uint32_t g_hashes_sent_upto = 0;  // under g_mutex: the newest frame whose hash went out

void write_hashes(Writer& w) {
    // A guest whose game becomes the host's from a snapshot has nothing to compare until then (in place
    // its frames so far were its own game's).
    if (g_snap_coming && !g_snap_tried) {
        w.u32(1);
        w.u8(0);
        return;
    }
    const uint32_t from = g_hashes_sent_upto > kHashRepeats ? g_hashes_sent_upto - kHashRepeats + 1 : 1;
    uint32_t count = std::min({g_own_hash_upto, kHashesPerPacket, g_own_hash_upto >= from ? g_own_hash_upto - from + 1 : 0u});
    if (g_own_hash_upto > g_hashes_sent_upto + kHashesPerPacket || g_own_hash_upto < g_hashes_sent_upto) {
        count = std::min(g_own_hash_upto, kHashesPerPacket);  // far behind, or a jump back (a snapshot)
    }
    g_hashes_sent_upto = g_own_hash_upto;
    uint32_t first = g_own_hash_upto - count + 1;
    // Only frames played here: after a snapshot the game went on from its frame, past ones it never had.
    while (count > 0 && g_own_hashes[first % kRing].frame != first) {
        first++;
        count--;
    }
    w.u32(first);
    w.u8(static_cast<uint8_t>(count));
    for (uint32_t frame = first; frame < first + count; frame++) {
        w.u64(g_own_hashes[frame % kRing].value);
    }
}

// A desync log holds this many frames at most (the last ones played), and the folder this many logs:
// a hostile host that drops a guest again and again, each time after a snapshot of a far frame, made
// it write a log of millions of lines on every pass.
constexpr uint32_t kHashLogFrames = kRing;
constexpr size_t kHashLogsKept = 16;

constexpr size_t kMemoryDumpsKept = 4;  // desync-....ram files, a few MB each

// Deletes the oldest desync files with that extension in folder, so that at most kept - 1 are left.
void prune_hash_logs(const std::filesystem::path& folder, const char* extension = ".txt", size_t kept = kHashLogsKept) {
    std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> logs;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
        const std::string name = entry.path().filename().string();
        if (entry.is_regular_file(error) && name.rfind("desync-", 0) == 0 && entry.path().extension() == extension) {
            logs.emplace_back(entry.last_write_time(error), entry.path());
        }
    }
    if (logs.size() < kept) {
        return;
    }
    std::sort(logs.begin(), logs.end());
    for (size_t i = 0; i + kept - 1 < logs.size(); i++) {
        std::filesystem::remove(logs[i].second, error);
    }
}

// The hashed game memory of the last kMemoryFrames frames played here (game thread, under
// g_copy_mutex). A desync writes them next to its hashes, so that two machines' files show the very
// bytes that differ (tools/net-test/compare_desync.py --ram), not only the 64 KiB blocks.
constexpr uint32_t kMemoryFrames = 16;
struct MemoryCopy {
    uint32_t frame = 0;
    std::vector<uint8_t> bytes;
};
std::mutex g_copy_mutex;
MemoryCopy g_memory_copies[kMemoryFrames];

void keep_memory(const uint8_t* rdram, uint32_t frame) {
    std::lock_guard lock(g_copy_mutex);
    MemoryCopy& copy = g_memory_copies[frame % kMemoryFrames];
    copy.bytes.assign(rdram + kHashStart, rdram + kHashEnd);
    copy.frame = frame;
}

// The kept frames to path, on a thread of its own: "CKRAM1", the start address (physical), the size
// and the count, then for each frame its number, the compressed size and zlib data, of the bytes
// themselves for the first and of their XOR with the frame before for the others.
void write_memory_dump(const std::filesystem::path& path) {
    std::vector<MemoryCopy> frames;
    {
        std::lock_guard lock(g_copy_mutex);
        for (const MemoryCopy& copy : g_memory_copies) {
            if (copy.frame != 0 && !copy.bytes.empty()) {
                frames.push_back(copy);
            }
        }
    }
    std::sort(frames.begin(), frames.end(), [](const MemoryCopy& a, const MemoryCopy& b) { return a.frame < b.frame; });
    std::thread([frames = std::move(frames), path] {
        FILE* file = std::fopen(path.string().c_str(), "wb");
        if (file == nullptr) {
            return;
        }
        const auto u32 = [&](uint32_t value) { std::fwrite(&value, 4, 1, file); };
        std::fwrite("CKRAM1", 1, 6, file);
        u32(kHashStart);
        u32(kHashEnd - kHashStart);
        u32(static_cast<uint32_t>(frames.size()));
        std::vector<uint8_t> delta(kHashEnd - kHashStart);
        std::vector<uint8_t> packed(compressBound(static_cast<uLong>(delta.size())));
        for (size_t i = 0; i < frames.size(); i++) {
            for (size_t b = 0; b < delta.size(); b++) {
                delta[b] = i == 0 ? frames[i].bytes[b] : frames[i].bytes[b] ^ frames[i - 1].bytes[b];
            }
            uLongf size = static_cast<uLongf>(packed.size());
            if (compress2(packed.data(), &size, delta.data(), static_cast<uLong>(delta.size()), 1) != Z_OK) {
                break;
            }
            u32(frames[i].frame);
            u32(static_cast<uint32_t>(size));
            std::fwrite(packed.data(), 1, size, file);
        }
        std::fclose(file);
        std::printf("[conker] net: this machine's game memory of its last %zu frames is in %s\n", frames.size(),
                    path.filename().string().c_str());
    }).detach();
}

// Under g_mutex: this machine's hashes of the last frames it played, to compare with the other
// machine's when a player went out of sync (conker-recomp-data/net/desync-...).
void write_hash_log(const char* role, int port, uint32_t frame) {
    const std::filesystem::path folder = std::filesystem::current_path() / "conker-recomp-data" / "net";
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    prune_hash_logs(folder);
    char name[96];
    std::snprintf(name, sizeof(name), "desync-%08x-p%d-f%u-%s.txt", g_session, port + 1, frame, role);
    FILE* file = std::fopen((folder / name).string().c_str(), "w");
    if (file == nullptr) {
        return;
    }
    const uint32_t from = g_own_hash_upto > kHashLogFrames ? g_own_hash_upto - kHashLogFrames + 1 : 1;
    for (uint32_t f = from; f <= g_own_hash_upto; f++) {
        if (const uint64_t* hash = own_hash(f)) {
            std::fprintf(file, "%u %016llx\n", f, static_cast<unsigned long long>(*hash));
        }
    }
    std::fprintf(file, "# 64 KiB block hashes from 0x%08x: b <frame> <block hashes> (tools/net-test/compare_desync.py)\n",
                 0x80000000u + kDetailFirst);
    for (uint32_t f = from; f <= g_own_hash_upto; f++) {
        const Detail& detail = g_details[f % kDetailFrames];
        if (detail.frame != f) {
            continue;
        }
        std::fprintf(file, "b %u", f);
        for (uint32_t block : detail.blocks) {
            std::fprintf(file, " %08x", block);
        }
        std::fputc('\n', file);
    }
    std::fclose(file);
    std::printf("[conker] net: this machine's hashes are in conker-recomp-data/net/%s\n", name);
    prune_hash_logs(folder, ".ram", kMemoryDumpsKept);
    write_memory_dump((folder / name).replace_extension(".ram"));
}

// The host: a guest's hashes.
void read_hashes(Reader& r, int port) {
    const uint32_t first = r.u32();
    const uint32_t count = std::min<uint32_t>(r.u8(), kHashesPerPacket);
    for (uint32_t frame = first; frame < first + count && r.ok; frame++) {
        const uint64_t value = r.u64();
        if (r.ok && frame != 0) {
            g_peer_hashes[port][frame % kRing] = {frame, value};
            g_peer_played[port] = std::max(g_peer_played[port], frame);
            compare_hashes(frame);
        }
    }
}

// --- lockstep --------------------------------------------------------------------------------------

void wake_game_thread() {
    if (g_queue_made.load() && g_waiting.load()) {
        ultramodern::enqueue_external_message(static_cast<int32_t>(kWaitQueue), 0, false, false);
    }
}

// The host, under g_mutex: merges every frame whose inputs are all in (a dropped player's are
// nothing pressed). True if any was.
bool merge() {
    bool merged = false;
    for (;;) {
        const uint32_t frame = g_merged_upto + 1;
        Merged record;
        record.frame = frame;
        record.held = g_ports;
        for (int port = 0; port < kPorts; port++) {
            if ((g_ports & (1 << port)) == 0) {
                continue;
            }
            if (dropped_at(port, frame)) {
                record.ports[port] = neutral_port();
                record.dropped |= 1 << port;
                continue;
            }
            const Input& input = g_inputs[port][frame % kRing];
            if (g_input_upto[port] >= frame && input.frame == frame) {
                record.ports[port] = normalized(input.port);
                record.mice[port] = limited(input.mouse);
                continue;
            }
            return merged;
        }
        if (g_snap_requested.exchange(false)) {
            record.snapshot = 1;
        }
        g_merged[frame % kRing] = record;
        g_history.push_back(record);
        g_merged_upto = frame;
        merged = true;
    }
}

// Under g_mutex: the ports whose input for frame is not in (the host), for "waiting for".
uint8_t missing_for(uint32_t frame) {
    uint8_t missing = 0;
    for (int port = 0; port < kPorts; port++) {
        if ((g_ports & (1 << port)) != 0 && g_input_upto[port] < frame && !dropped_at(port, frame)) {
            missing |= 1 << port;
        }
    }
    return missing;
}

// Under g_mutex: the packets this side sends each tick (and when it has something new).
// whole: the first record of each packet whole (the tick's resends), so that a receiver without the
// frame before it (after a snapshot, say) gets going within a tick; otherwise as changes from it.
void send_updates(bool whole = false) {
    if (is_host()) {
        for (Peer& peer : g_peers) {
            if (!peer.started) {
                continue;
            }
            Writer w;
            begin(w, kMerged, peer.secret);
            write_times(w, peer);
            w.u32(g_input_upto[peer.port]);
            w.u8(missing_for(g_merged_upto + 1));
            w.u32(g_merged_upto);
            const uint32_t first = std::max(peer.acked + 1, static_cast<uint32_t>(g_delay + 1));
            // A dropped guest gets no more records, only the drops, so that one out of sync learns it;
            // a rejoining one gets them from the start, from the history.
            const uint32_t count = (!peer.dropped || peer.rejoining) && first <= g_merged_upto
                                       ? std::min(g_merged_upto - first + 1, kFramesPerPacket) : 0;
            w.u32(first);
            w.u8(static_cast<uint8_t>(count));
            // Each record as its changes from the one before: a mask (bit p for port p, bit 4 for the
            // held/dropped/snapshot bytes) then what it names; the first in the packet whole. A record
            // was 71 bytes and up to 16 went 100 times a second, repeated until acknowledged, ~1-2
            // Mbit/s once the round trip grew, which filled a phone hotspot's buffer (650 ms round trips).
            std::array<uint8_t, kPortBytes> before[kPorts];
            uint8_t before_tail[3] = {};
            const bool after_base = !whole && first > static_cast<uint32_t>(g_delay) + 1;
            if (after_base) {
                const Merged& base = g_history[first - static_cast<uint32_t>(g_delay) - 2];
                for (int port = 0; port < kPorts; port++) {
                    before[port] = port_bytes(base.ports[port], base.mice[port]);
                }
                before_tail[0] = base.held;
                before_tail[1] = base.dropped;
                before_tail[2] = base.snapshot;
            }
            for (uint32_t frame = first; frame < first + count; frame++) {
                const Merged& record = g_history[frame - static_cast<uint32_t>(g_delay) - 1];
                const uint8_t tail[3] = {record.held, record.dropped, record.snapshot};
                const bool all = frame == first && !after_base;
                std::array<uint8_t, kPortBytes> now[kPorts];
                uint8_t mask = frame == first && after_base ? kAfterBase : 0;
                for (int port = 0; port < kPorts; port++) {
                    now[port] = port_bytes(record.ports[port], record.mice[port]);
                    if (all || now[port] != before[port]) {
                        mask |= static_cast<uint8_t>(1 << port);
                    }
                }
                if (all || std::memcmp(tail, before_tail, 3) != 0) {
                    mask |= kTailChanged;
                }
                w.u8(mask);
                for (int port = 0; port < kPorts; port++) {
                    if ((mask & (1 << port)) != 0) {
                        w.bytes(now[port].data(), kPortBytes);
                    }
                    before[port] = now[port];
                }
                if ((mask & kTailChanged) != 0) {
                    w.bytes(tail, 3);
                }
                std::memcpy(before_tail, tail, 3);
            }
            // The drops, only while there is one to tell: most packets had 36 bytes of zeros.
            bool any = false;
            for (int port = 0; port < kPorts; port++) {
                any = any || g_drop_frame[port] != 0 || g_drop_reason[port] != 0 || g_live_frame[port] != 0;
            }
            w.u8(any ? 1 : 0);
            for (int port = 0; port < kPorts && any; port++) {
                w.u32(g_drop_frame[port]);
                w.u8(g_drop_reason[port]);
                w.u32(g_live_frame[port]);
            }
            send_to(peer, w);
        }
        return;
    }
    if (g_peers.empty() || g_stopped.load() != kPlaying) {
        return;
    }
    Peer& host = g_peers[0];
    Writer w;
    begin(w, kInput, g_secret);
    w.u8(static_cast<uint8_t>(g_local_port));
    write_times(w, host);
    w.u32(g_merged_upto);
    const uint32_t upto = g_input_upto[g_local_port];
    const uint32_t first = std::max(host.acked + 1, static_cast<uint32_t>(g_delay + 1));
    const uint32_t count = first <= upto ? std::min(upto - first + 1, kFramesPerPacket) : 0;
    w.u32(first);
    w.u8(static_cast<uint8_t>(count));
    // As MERGED's records: a 1 then the port when it changed from the frame before (always the first),
    // else a 0.
    // The first: a 2 when it is the same as frame first - 1's (which the host has acknowledged), unless
    // whole.
    std::array<uint8_t, kPortBytes> before{};
    const Input& base = g_inputs[g_local_port][(first - 1) % kRing];
    const bool after_base = !whole && first > static_cast<uint32_t>(g_delay) + 1 && base.frame == first - 1;
    if (after_base) {
        before = port_bytes(base.port, base.mouse);
    }
    for (uint32_t frame = first; frame < first + count; frame++) {
        const Input& input = g_inputs[g_local_port][frame % kRing];
        const std::array<uint8_t, kPortBytes> now = port_bytes(input.port, input.mouse);
        const bool changed = (frame == first && !after_base) || now != before;
        w.u8(changed ? 1 : frame == first ? 2 : 0);
        if (changed) {
            w.bytes(now.data(), kPortBytes);
        }
        before = now;
    }
    write_hashes(w);
    send_to(host, w);
}

Peer* peer_from(const sockaddr_storage& addr, socklen_t len) {
    for (Peer& peer : g_peers) {
        if (same_address(peer.addr, peer.addr_len, addr, len)) {
            return &peer;
        }
    }
    return nullptr;
}

// The host (under g_mutex once the network thread runs): a datagram from the session's socket opened
// into `plain`. False if there is nothing more to do with it: an ASK (answered here), a packet in the
// clear, or a packet that does not check out on its sender's link. A FIRST from an address
// no peer has opens on a link of its own, kept in g_contact for the HELLO it carries: *contact says so.
bool host_open(const uint8_t* data, size_t size, const sockaddr_storage& from, socklen_t from_len, uint8_t* plain,
               size_t capacity, size_t* plain_size, bool* contact) {
    *contact = false;
    const uint8_t kind = seal::kind_of(data, size);
    if (kind == seal::kAsk) {
        if (seal::is_ask(data, size)) {
            send_key_offer(from, from_len);
        }
        return false;
    }
    if (Peer* peer = peer_from(from, from_len)) {
        return seal::open(peer->channel, data, size, plain, capacity, plain_size);
    }
    uint64_t cookie = 0;
    if (!seal::first_cookie(data, size, &cookie)) {
        return false;
    }
    // The cookie in the clear first: the key exchange is work, and a forged sender never saw a cookie.
    if (!conker::net::address_cookie_ok(from, kCookiePurpose, cookie)) {
        send_key_offer(from, from_len);  // one too old: a new one
        return false;
    }
    if (!seal::open_first(g_host_key, data, size, &g_contact, plain, capacity, plain_size)) {
        return false;
    }
    *contact = true;
    return true;
}

// A guest, under g_mutex: a datagram from the host opened on its link into `plain`; false if it is
// not from the host or does not check out.
bool guest_open(const uint8_t* data, size_t size, const sockaddr_storage& from, socklen_t from_len, uint8_t* plain,
                size_t capacity, size_t* plain_size) {
    Peer* host = peer_from(from, from_len);
    return host != nullptr && seal::kind_of(data, size) == seal::kSealed &&
           seal::open(host->channel, data, size, plain, capacity, plain_size);
}

// The host, under g_mutex: a guest's INPUT.
void on_input(Peer& peer, Reader& r) {
    const int port = r.u8();
    if (port != peer.port) {
        return;
    }
    read_times(r, peer);
    const uint32_t acked = r.u32();
    const uint32_t first = r.u32();
    const uint32_t count = std::min<uint32_t>(r.u8(), kFramesPerPacket);
    if (!r.ok) {
        return;
    }
    peer.started = true;
    if (peer.dropped && !peer.rejoining) {
        return;
    }
    peer.acked = std::max(peer.acked, std::min(acked, g_merged_upto));
    const uint32_t had = g_input_upto[port];
    Port p;
    Mouse m;
    for (uint32_t frame = first; frame < first + count; frame++) {
        const uint8_t changed = r.u8();
        if (changed > 2 || (frame == first && changed == 0) || (frame != first && changed == 2)) {
            return;  // not this build's packet
        }
        if (changed == 2) {
            // The same as frame first - 1, which this machine has unless a resend of it is still to come.
            const Input& base = g_inputs[port][(first - 1) % kRing];
            if (base.frame != first - 1 || first - 1 > g_input_upto[port]) {
                return;
            }
            p = base.port;
            m = base.mouse;
        }
        if (changed == 1) {
            read_port(r, &p, &m);
        }
        if (!r.ok) {
            return;
        }
        // Only the next frame, or one already in: the ring holds no more than kRing ahead. A rejoining
        // player's input counts from its live frame on.
        if (!peer.dropped && frame == g_input_upto[port] + 1 && frame < g_merged_upto + kRing / 2) {
            g_inputs[port][frame % kRing] = {frame, p, m};
            g_input_upto[port] = frame;
        }
    }
    if (g_input_upto[port] != had) {
        peer.moved_on = now_ms();
    }
    read_hashes(r, port);
    if (merge()) {
        send_updates();
        wake_game_thread();
    }
}

// A guest, under g_mutex: the host's MERGED.
void on_merged(Peer& host, Reader& r) {
    read_times(r, host);
    const uint32_t acked = r.u32();
    g_host_waiting_for = r.u8() & static_cast<uint8_t>((1 << kPorts) - 1);  // only real port bits
    const uint32_t host_upto = r.u32();
    const uint32_t first = r.u32();
    const uint32_t count = std::min<uint32_t>(r.u8(), kFramesPerPacket);
    if (!r.ok) {
        return;
    }
    if (host_upto <= kMaxFrame) {
        g_host_upto = std::max(g_host_upto, host_upto);
    }
    host.acked = std::max(host.acked, std::min(acked, g_input_upto[g_local_port]));
    bool advanced = false;
    Merged record;  // each record starts from the one before it (send_updates)
    constexpr uint8_t kAll = static_cast<uint8_t>((1 << kPorts) - 1) | kTailChanged;
    for (uint32_t frame = first; frame < first + count; frame++) {
        record.frame = frame;
        uint8_t mask = r.u8();
        if (frame == first && (mask & kAfterBase) != 0) {
            // Changes from frame first - 1: this machine's own, unless it does not have it (the tick's
            // next resend comes whole).
            const Merged& base = g_merged[(first - 1) % kRing];
            if (base.frame != first - 1 || first - 1 > g_merged_upto) {
                return;
            }
            record = base;
            record.frame = frame;
            mask &= static_cast<uint8_t>(~kAfterBase);
        } else if ((mask & ~kAll) != 0 || (frame == first && mask != kAll)) {
            return;  // not this build's packet
        }
        for (int port = 0; port < kPorts; port++) {
            if ((mask & (1 << port)) != 0) {
                read_port(r, &record.ports[port], &record.mice[port]);
            }
        }
        if ((mask & kTailChanged) != 0) {
            record.held = r.u8() & static_cast<uint8_t>((1 << kPorts) - 1);  // only real port bits
            record.dropped = r.u8() & static_cast<uint8_t>((1 << kPorts) - 1);
            record.snapshot = r.u8();
        }
        if (!r.ok) {
            return;
        }
        // In order, and no further ahead of the game here than the ring holds: catching up, the
        // records come far faster than the game plays them.
        if (frame == g_merged_upto + 1 && frame < g_own_hash_upto + kRing / 2 && (!g_snap_coming || g_snap_tried)) {
            g_merged[frame % kRing] = record;
            g_merged_upto = frame;
            advanced = true;
        }
    }
    const bool drops = r.u8() != 0;  // none to tell: every port's drop frame, reason and live frame are 0
    for (int port = 0; port < kPorts && r.ok; port++) {
        const uint32_t from = drops ? r.u32() : 0;
        const uint8_t reason = drops ? r.u8() : 0;
        const uint32_t live = drops ? r.u32() : 0;
        if (!r.ok) {
            continue;
        }
        // A hostile host's drop fields are untrusted: ignore a port whose frames or reason are out of
        // range rather than painting the drop state or overlay from them.
        if (from > kMaxFrame || live > kMaxFrame || reason > kBack) {
            continue;
        }
        if (from != 0 && from == g_drop_frame[port] && reason != g_drop_reason[port] &&
            (reason == kBack || reason == kLeft) && (g_drop_reason[port] == kBack || g_drop_reason[port] == kLeft)) {
            g_drop_reason[port] = reason;  // the dropped player coming back, or not after all (for the note)
        }
        if (live != g_live_frame[port] && from == g_drop_frame[port]) {
            g_live_frame[port] = live;
            std::printf("[conker] net: player %d plays again from frame %u\n", port + 1, live);
        }
        if (from == 0 || from == g_drop_frame[port]) {
            continue;
        }
        g_drop_frame[port] = from;
        g_drop_reason[port] = reason;
        g_live_frame[port] = live;
        if (port != g_local_port) {
            if (reason == kNew) {
                std::printf("[conker] net: player %d joins the session from frame %u (plugged in outside a match)\n",
                            port + 1, from);
            } else {
                std::printf("[conker] net: player %d dropped from frame %u (%s)\n", port + 1, from,
                            reason == kOutOfSync ? "out of sync" : "gone");
            }
        } else if (from == g_rejoin_from) {
            // This machine's own earlier drop, which it is rejoining from.
        } else if (g_leave_requested.load()) {
            // The host taking this machine's LEAVE: it is leaving anyway, not dropped.
        } else {
            // Dropped as out of sync, or as gone (not heard from, or its game stuck, which it may get
            // over): the host plays this port with nothing pressed, so the game here stops.
            int playing = kPlaying;
            if (g_stopped.compare_exchange_strong(playing, reason == kOutOfSync ? kStoppedOutOfSync : kStoppedDropped)) {
                g_stopped_frame = from;
                if (reason == kOutOfSync) {
                    std::fprintf(stderr, "[conker] net: OUT OF SYNC: the host dropped this machine from frame %u, its "
                                         "game memory no longer matching the host's. The game here has stopped.\n", from);
                    write_hash_log("guest", g_local_port, from);
                } else {
                    std::fprintf(stderr, "[conker] net: DROPPED: the host dropped this machine from frame %u, having "
                                         "waited too long for it. The game here has stopped.\n", from);
                }
            }
            advanced = true;
        }
    }
    if (advanced) {
        host.moved_on = now_ms();
        wake_game_thread();
    }
}

ssize_t receive(uint8_t* buffer, size_t size, sockaddr_storage* from, socklen_t* from_len, int timeout_ms);
std::vector<uint8_t> session_blob(int port, uint32_t rejoin_from, bool snapshot);
std::string build_text();
std::string os_text();
std::filesystem::path data_dir();

// The host, under g_mutex: a HELLO after the start, from a machine coming back for a dropped port
// (the one it asks for, or the first dropped), or a newcomer, who takes a port nobody has held
// (when no dropped one is free); in a lobby's session (--net-token) only a machine with the token, or
// a dropped player's own (its secret). It gets the session as at the start and a snapshot of the host's
// game (or the merged records from frame 1); once it has nearly caught up it plays its port. A
// newcomer's port is held from the first frame not merged yet (Merged::held), with nothing pressed
// until it plays, and reads as unplugged until the game is outside a match (frame()): a character
// cannot join a match under way, so the newcomer comes in at the next join screen, where Start
// takes the port in as on a console.
void on_rejoin_hello(const sockaddr_storage& from, socklen_t from_len, Reader& r, uint64_t secret) {
    const std::string build = r.text(200);
    r.text(64);  // its OS and CPU: any (net_lobby.cpp's mismatch says why)
    const uint64_t rom = r.u64();
    const int wanted = r.u8();
    const bool no_snapshot = r.u8() != 0;
    const uint64_t cookie = r.u64();
    const uint64_t token = r.u64();  // the lobby's: a session that has one lets in no one without it
    if (!r.ok || secret == 0) {
        return;
    }
    if (!conker::net::address_cookie_ok(from, kCookiePurpose, cookie)) {
        Writer w;
        begin(w, kCookie, secret);
        w.u64(conker::net::address_cookie(from, kCookiePurpose));
        send_contact(from, from_len, w);
        return;
    }
    // Its own port, if this machine had one and was dropped (its secret says so): even while it is coming
    // back, from another address. Else a free port, as a newcomer (below); else a port whose player
    // has been gone a long time and is not coming back.
    const int64_t now = now_ms();
    Peer* chosen = nullptr;
    bool own = false;
    for (Peer& peer : g_peers) {
        if (peer.dropped && conker::net::same_secret(peer.secret, secret) && g_desyncs[peer.port] < kMaxDesyncs && g_drops[peer.port] < kMaxDrops) {
            chosen = &peer;
            own = true;
        }
    }
    std::string problem;
    if (!r.ok) {
        problem = "a bad HELLO";
    } else if (build != build_text()) {
        problem = "the host runs build " + build_text() + ", this is " + conker::net::printable(build, 40);
    } else if (rom != conker::net::rom_hash()) {
        problem = "the host plays another ROM";
    } else if (g_options.token != 0 && !conker::net::same_secret(token, g_options.token)) {
        // Before the start and after: a newcomer, a port taken over, and a dropped player's own machine
        // need the token too. Without it a stranger who reached the port took a seat and got the host's
        // save; and a secret seen once, the same for every session at that address, got a
        // dropped port back in later sessions without the token. The machine starting
        // itself again to rejoin keeps its command line, token and all.
        problem = "this session lets in only its own players (its lobby's, or those given its --net-token)";
    }
    bool newcomer = false;
    if (problem.empty() && chosen == nullptr) {
        for (Peer& peer : g_peers) {
            if (peer.dropped && !peer.rejoining && now - peer.dropped_at >= kReclaimAfterMs &&
                (chosen == nullptr || peer.port == wanted - 1) && g_desyncs[peer.port] < kMaxDesyncs &&
                g_drops[peer.port] < kMaxDrops) {
                chosen = &peer;
            }
        }
    }
    if (problem.empty() && chosen == nullptr) {
        int port = wanted - 1 >= 1 && wanted - 1 < kPorts && (g_ports & (1 << (wanted - 1))) == 0 ? wanted - 1 : 1;
        while (port < kPorts && (g_ports & (1 << port)) != 0) {
            port++;
        }
        if (port < kPorts) {
            Peer joined;
            joined.port = port;
            joined.dropped = true;
            joined.dropped_at = now;
            g_peers.push_back(joined);
            chosen = &g_peers.back();
            g_ports |= static_cast<uint8_t>(1 << port);
            g_drop_frame[port] = g_merged_upto + 1;
            g_drop_reason[port] = kNew;
            g_live_frame[port] = 0;
            newcomer = true;
        }
    }
    if (problem.empty() && chosen == nullptr) {
        // A dropped port whose player has not been gone long enough is held for that player's machine.
        for (Peer& peer : g_peers) {
            if (peer.dropped && !peer.rejoining && now - peer.dropped_at < kReclaimAfterMs && g_desyncs[peer.port] < kMaxDesyncs &&
                g_drops[peer.port] < kMaxDrops) {
                problem = "the session is full; player " + std::to_string(peer.port + 1) + "'s place is kept for them for " +
                          std::to_string((kReclaimAfterMs - (now - peer.dropped_at)) / 1000 + 1) + " s more";
            }
        }
    }
    if (problem.empty() && chosen == nullptr) {
        bool banned = false;
        for (const Peer& peer : g_peers) {
            banned = banned || (peer.dropped && (g_desyncs[peer.port] >= kMaxDesyncs || g_drops[peer.port] >= kMaxDrops));
        }
        problem = banned ? "the session has started; that player was dropped too often (out of sync twice, or four "
                           "drops), so cannot rejoin"
                         : "the session is full";
    }
    if (!problem.empty()) {
        if (conker::net::log_refusal()) {
            std::printf("[conker] net: %s turned away: %s\n", address_text(from, from_len).c_str(), problem.c_str());
        }
        Writer w;
        begin(w, kReject, secret);
        w.text(problem, 200);
        send_contact(from, from_len, w);
        return;
    }
    Peer& peer = *chosen;
    peer.addr = from;
    peer.addr_len = from_len;
    peer.secret = secret;
    peer.channel = g_contact;
    peer.last_heard = now_ms();
    peer.moved_on = 0;
    peer.acked = 0;
    peer.echo = 0;
    peer.rtt = -1;
    peer.ready = false;
    peer.started = false;
    peer.rejoining = true;
    if (own && g_drop_reason[peer.port] == kLeft) {
        g_drop_reason[peer.port] = kBack;
    }
    const bool snapshot = !no_snapshot && !g_snap_failed[peer.port] && std::getenv("CONKER_NET_NO_SNAPSHOT") == nullptr;
    peer.blob = session_blob(peer.port, g_drop_frame[peer.port], snapshot);
    // Its game is the host's only once the snapshot is in place (a machine rejoining in place has not
    // played the session's frames at all).
    peer.needs_snapshot = snapshot;
    peer.snap_wanted = 0;
    peer.snap = nullptr;
    peer.used_snapshot = false;
    for (Hash& hash : g_peer_hashes[peer.port]) {
        hash = Hash{};
    }
    g_peer_played[peer.port] = 0;
    std::printf("[conker] net: player %d %s from %s, %s (the host is at frame %u)\n", peer.port + 1,
                newcomer ? "joins the running session (a port nobody held; plugged in outside a match)"
                         : own ? "is back" : "goes to a newcomer (its player has been gone a long time)",
                address_text(from, from_len).c_str(), snapshot ? "starting from a snapshot" : "catching up from frame 1",
                g_merged_upto);
    update_offer();
}

// The host, under g_mutex, every tick: a rejoining guest's session and START, and its live frame
// once it has nearly caught up.
int64_t g_rejoin_tick_chunks = 0;  // under g_mutex: when rejoin_tick last sent a session's chunks

void rejoin_tick(int64_t now) {
    const bool chunks_due = now - g_rejoin_tick_chunks >= 100;
    if (chunks_due) {
        g_rejoin_tick_chunks = now;
    }
    for (Peer& peer : g_peers) {
        if (!peer.rejoining) {
            continue;
        }
        if (!peer.ready && chunks_due) {
            for (size_t offset = 0; offset < peer.blob.size(); offset += kChunkSize) {
                Writer w;
                begin(w, kChunk, peer.secret);
                w.u32(static_cast<uint32_t>(now));
                w.u32(static_cast<uint32_t>(peer.blob.size()));
                w.u32(static_cast<uint32_t>(offset));
                const size_t length = std::min(kChunkSize, peer.blob.size() - offset);
                w.u16(static_cast<uint16_t>(length));
                w.bytes(peer.blob.data() + offset, length);
                send_to(peer, w);
            }
        } else if (peer.ready && !peer.started && chunks_due) {
            Writer w;
            begin(w, kStart, peer.secret);
            w.u8(static_cast<uint8_t>(g_delay));
            send_to(peer, w);
        } else if (peer.started && g_peer_played[peer.port] + kLiveWithin >= g_merged_upto &&
                   (!peer.needs_snapshot || (peer.snap != nullptr && g_peer_played[peer.port] >= peer.snap_frame))) {
            const uint32_t live = g_merged_upto + kLiveLead;
            g_live_frame[peer.port] = live;
            g_input_upto[peer.port] = live - 1;
            peer.dropped = false;
            peer.rejoining = false;
            peer.used_snapshot = false;
            peer.needs_snapshot = false;
            peer.snap = nullptr;
            peer.moved_on = now;
            std::printf("[conker] net: player %d has caught up (frame %u of %u) and plays again from frame %u\n",
                        peer.port + 1, g_peer_played[peer.port], g_merged_upto, live);
            update_offer();
        }
    }
}

// The host, under g_mutex: which of its snapshot's chunks a rejoining guest has.
void on_snap_ack(Peer& peer, Reader& r) {
    const uint32_t frame = r.u32();
    const uint32_t count = r.u32();
    if (!r.ok || peer.snap == nullptr || frame != peer.snap_frame || count != peer.snap_acked.size()) {
        return;
    }
    size_t have = 0;
    for (uint32_t i = 0; i < count && r.ok; i += 8) {
        const uint8_t bits = r.u8();
        for (uint32_t b = 0; b < 8 && i + b < count; b++) {
            if ((bits >> b) & 1) {
                peer.snap_acked[i + b] = true;
            }
        }
    }
    for (bool acked : peer.snap_acked) {
        have += acked ? 1 : 0;
    }
    if (have == peer.snap_acked.size() && peer.snap_since != 0) {
        std::printf("[conker] net: player %d has the snapshot of frame %u (%zu KiB) after %lld ms\n", peer.port + 1,
                    peer.snap_frame, peer.snap->size() / 1024, static_cast<long long>(now_ms() - peer.snap_since));
        peer.snap_since = 0;
    }
}

// A guest, under g_mutex: a chunk of the host's snapshot.
void on_snap_chunk(Reader& r) {
    const uint32_t frame = r.u32();
    const uint32_t total = r.u32();
    const uint32_t offset = r.u32();
    const uint16_t length = r.u16();
    // In 64 bits, and each chunk exactly the size its place implies: a sum in 32 bits wraps (a chunk at
    // 0xFFFFFC00 passed, and wrote 4 GiB past the buffer).
    // No bigger than an 8 MiB snapshot packs to (and its size word), and the first chunk's snapshot
    // is the one: the host sends one, so a chunk of another frame or size does not start it again.
    static const uint32_t max_total = static_cast<uint32_t>(compressBound(conker::snapshot::kMaxSize)) + 4;
    if (!r.ok || !g_snap_coming || g_snap_tried || !chunk_fits(total, max_total, offset, length, kSnapChunkSize) ||
        r.at + length > r.size) {
        return;
    }
    if (!g_snap_have.empty() && (frame != g_snap_of || g_snap_data.size() != total)) {
        return;
    }
    if (g_snap_have.empty()) {
        g_snap_of = frame;
        g_snap_data.assign(total, 0);
        g_snap_have.assign((total + kSnapChunkSize - 1) / kSnapChunkSize, false);
        g_snap_have_count = 0;
    }
    const size_t index = offset / kSnapChunkSize;
    if (!g_snap_have[index]) {
        g_snap_have[index] = true;
        g_snap_have_count++;
        std::memcpy(g_snap_data.data() + offset, r.data + r.at, length);
        if (g_snap_have_count == g_snap_have.size()) {
            wake_game_thread();
        }
    }
}

// The host, under g_mutex, every tick: a rejoining guest's snapshot, once made, in chunks.
void snapshot_tick() {
    for (Peer& peer : g_peers) {
        if (!peer.rejoining || !peer.started) {
            continue;
        }
        if (peer.snap == nullptr && peer.snap_wanted != 0 && g_snap_serial >= peer.snap_wanted && g_snap_blob != nullptr) {
            peer.snap = g_snap_blob;
            peer.snap_frame = g_snap_frame;
            peer.snap_acked.assign((peer.snap->size() + kSnapChunkSize - 1) / kSnapChunkSize, false);
            peer.snap_next = 0;
            peer.snap_since = now_ms();
            peer.used_snapshot = true;
        }
        if (peer.snap == nullptr) {
            continue;
        }
        const size_t chunks = peer.snap_acked.size();
        for (int sent = 0, looked = 0; sent < kSnapChunksPerTick && looked < static_cast<int>(chunks); looked++) {
            const size_t index = peer.snap_next;
            peer.snap_next = (peer.snap_next + 1) % chunks;
            if (peer.snap_acked[index]) {
                continue;
            }
            const size_t offset = index * kSnapChunkSize;
            const size_t length = std::min(kSnapChunkSize, peer.snap->size() - offset);
            Writer w;
            begin(w, kSnapChunk, peer.secret);
            w.u32(peer.snap_frame);
            w.u32(static_cast<uint32_t>(peer.snap->size()));
            w.u32(static_cast<uint32_t>(offset));
            w.u16(static_cast<uint16_t>(length));
            w.bytes(peer.snap->data() + offset, length);
            send_to(peer, w);
            sent++;
        }
    }
}

// A guest, under g_mutex, every tick while its game waits for the host's snapshot: asks for it, and
// says which chunks it has.
int64_t g_guest_last_want = 0;  // under g_mutex: when the guest last asked for the host's snapshot

void snapshot_guest_tick(int64_t now) {
    if (!g_snap_waiting.load() || g_peers.empty()) {
        return;
    }
    if (g_snap_have.empty() && now - g_guest_last_want >= 200) {
        g_guest_last_want = now;
        Writer w;
        begin(w, kSnapWant, g_secret);
        send_to(g_peers[0], w);
    }
    if (!g_snap_have.empty()) {
        Writer w;
        begin(w, kSnapAck, g_secret);
        w.u32(g_snap_of);
        w.u32(static_cast<uint32_t>(g_snap_have.size()));
        for (size_t i = 0; i < g_snap_have.size(); i += 8) {
            uint8_t bits = 0;
            for (size_t b = 0; b < 8 && i + b < g_snap_have.size(); b++) {
                bits |= g_snap_have[i + b] ? (1 << b) : 0;
            }
            w.u8(bits);
        }
        send_to(g_peers[0], w);
    }
}

// A guest whose game stopped because the host dropped it comes back: the process starts again with
// the same command line and asks for its port (CONKER_NET_REJOIN_PORT), then catches up. At most
// kMaxRelaunches times in a row (CONKER_NET_REJOIN_COUNT counts them through the relaunches): the host
// decides the drops, and a hostile one kept a guest starting again for ever. An honest
// host refuses a player dropped kMaxDrops times sooner.
constexpr int kMaxRelaunches = kMaxDrops;

bool g_relaunch_started = false;  // under g_mutex: relaunch_to_rejoin has acted for this drop

void relaunch_to_rejoin() {
    if (g_relaunch_started || is_host() || (g_argv.empty() && !g_in_place) || std::getenv("CONKER_NET_NO_REJOIN") != nullptr) {
        return;
    }
    const int stop = g_stopped.load();
    if (stop != kStoppedOutOfSync && stop != kStoppedDropped) {
        return;
    }
    g_relaunch_started = true;
    if (g_in_place) {
        // The game thread connects again (rejoin_in_place), the game going on meanwhile.
        if (g_rejoin_count >= kMaxRelaunches) {
            std::printf("[conker] net: dropped by the host %d times in a row: not rejoining\n", g_rejoin_count + 1);
            return;
        }
        g_rejoin_requested.store(true);
        return;
    }
    const char* count_text = std::getenv("CONKER_NET_REJOIN_COUNT");
    const int count = count_text != nullptr ? std::atoi(count_text) : 0;
    if (count >= kMaxRelaunches) {
        std::printf("[conker] net: dropped by the host %d times in a row: not starting again to rejoin (start the "
                    "game again to try)\n", count + 1);
        return;
    }
    std::thread([count] {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::printf("[conker] net: starting again to rejoin the session as player %d\n", g_local_port + 1);
        const std::string port = std::to_string(g_local_port + 1);
        process::set_env("CONKER_NET_REJOIN_PORT", port.c_str());
        char secret[17];
        std::snprintf(secret, sizeof(secret), "%016llx", static_cast<unsigned long long>(g_secret));
        process::set_env("CONKER_NET_REJOIN_SECRET", secret);
        process::set_env("CONKER_NET_REJOIN_COUNT", std::to_string(count + 1).c_str());
        if (std::getenv("CONKER_NET_TEST_KEEP") == nullptr) {
            process::unset_env("CONKER_NET_TEST_DESYNC");  // the forced trouble happened once; the replay plays clean
            process::unset_env("CONKER_NET_TEST_HANG");
        }
        process::start_again(g_argv);
    }).detach();
}

// A datagram from the session's socket (not the lobby's PROBE), sealed (net_seal.hpp). Every type but
// a HELLO from a new address needs the session id and the sender's secret.
void handle_datagram(const uint8_t* data, size_t size, const sockaddr_storage& from, socklen_t from_len) {
    std::lock_guard lock(g_mutex);
    uint8_t buffer[kMaxPacket + 64];
    size_t got = 0;
    bool contact = false;
    if (is_host() ? !host_open(data, size, from, from_len, buffer, sizeof(buffer), &got, &contact)
                  : !guest_open(data, size, from, from_len, buffer, sizeof(buffer), &got)) {
        return;
    }
    Reader r{buffer, got};
    if (r.u32() != kMagic) {
        return;
    }
    const uint8_t type = r.u8();
    const uint32_t session = r.u32();
    const uint64_t secret = r.u64();
    if (contact) {
        if (type == kHello && r.ok) {
            on_rejoin_hello(from, from_len, r, secret);
        }
        return;
    }
    Peer* peer = peer_from(from, from_len);
    if (!r.ok || peer == nullptr || session != g_session || !conker::net::same_secret(secret, is_host() ? peer->secret : g_secret)) {
        return;
    }
    peer->last_heard = now_ms();
    if (is_host() && type == kLeave) {
        if (!peer->dropped || peer->rejoining) {
            drop(*peer, kLeft, "it left");
        }
        return;
    }
    if (!is_host() && type == kLeave) {
        int playing = kPlaying;
        if (g_stopped.compare_exchange_strong(playing, kHostLeft)) {
            std::fprintf(stderr, "[conker] net: the host left: the session is over and the game here has stopped\n");
            wake_game_thread();
        }
        return;
    }
    if (is_host() && type == kInput) {
        on_input(*peer, r);
    } else if (is_host() && type == kReady) {
        const uint32_t sent = r.u32();
        if (r.ok && peer->rejoining && !peer->ready) {
            peer->ready = true;
            peer->rtt = rtt_from_echo(sent);
        }
    } else if (is_host() && type == kSnapWant) {
        if (peer->rejoining && peer->started && peer->snap_wanted == 0 && peer->snap == nullptr) {
            peer->snap_wanted = g_snap_serial + 1;  // the next one made
            g_snap_requested.store(true);
        }
    } else if (is_host() && type == kSnapAck) {
        on_snap_ack(*peer, r);
    } else if (!is_host() && type == kSnapChunk) {
        on_snap_chunk(r);
    } else if (is_host() && type == kStarted) {
        peer->started = true;
    } else if (!is_host() && type == kMerged) {
        on_merged(*peer, r);
    } else if (!is_host() && type == kStart) {
        Writer w;
        begin(w, kStarted, g_secret);
        send_to(*peer, w);
    }
}

void network_thread() {
    int64_t last_send = 0;
    int64_t last_update = 0;
    uint8_t buffer[kMaxPacket + 64];
    while (!g_net_stop.load()) {
        for (int wait = static_cast<int>(kSendEveryMs);; wait = 0) {
            sockaddr_storage from{};
            socklen_t from_len = sizeof(from);
            const ssize_t got = receive(buffer, sizeof(buffer), &from, &from_len, wait);
            if (got <= 0) {
                break;
            }
            if (is_host() && conker::net::lobby::answer_probe(g_socket, buffer, static_cast<size_t>(got), from,
                                                              from_len, session_offer())) {
                continue;
            }
            handle_datagram(buffer, static_cast<size_t>(got), from, from_len);
        }
        const int64_t now = now_ms();
        std::lock_guard lock(g_mutex);
        for (Peer& peer : g_peers) {
            // A machine coming back that has gone quiet: nothing more is sent to it (its port stays dropped).
            if (is_host() && peer.dropped && peer.rejoining && now - peer.last_heard >= kDropAfterMs) {
                std::printf("[conker] net: player %d's machine (%s) stopped answering while coming back; its port stays "
                            "dropped\n", peer.port + 1, address_text(peer.addr, peer.addr_len).c_str());
                peer.rejoining = false;
                peer.blob.clear();
                peer.snap = nullptr;
                peer.used_snapshot = false;
                if (g_drop_reason[peer.port] == kBack) {
                    g_drop_reason[peer.port] = kLeft;
                }
                update_offer();
            }
            if (is_host() && !peer.dropped && peer.started && now - peer.last_heard >= kQuietGraceMs &&
                (missing_for(g_merged_upto + 1) & (1 << peer.port)) != 0) {
                drop(peer, kLeft, "not heard from while the others waited");
                continue;
            }
            if (peer.dropped || now - peer.last_heard < kDropAfterMs || (!is_host() && g_stopped.load() != kPlaying)) {
                continue;
            }
            if (is_host()) {
                drop(peer, kLeft, "not heard from");
                continue;
            }
            int playing = kPlaying;
            if (g_stopped.load() == kPlaying && now - peer.last_heard >= kDropAfterMs &&
                g_stopped.compare_exchange_strong(playing, kHostLeft)) {
                std::fprintf(stderr, "[conker] net: the host has not been heard from for %lld s: the session is over "
                                     "and the game here has stopped\n", static_cast<long long>(kDropAfterMs / 1000));
                wake_game_thread();
            }
        }
        // A player heard from but whose game has stopped moving on while the others wait for it: the
        // host has been held up by its input for kStuckAfterMs (counted from when the host began to
        // wait, not from the guest's last input: a guest back from a drop sends none until its live
        // frame, which the host reaches a few seconds later).
        for (Peer& peer : g_peers) {
            if (peer.dropped || peer.moved_on == 0 || g_stopped.load() != kPlaying) {
                continue;
            }
            if (is_host()) {
                const uint32_t next = g_merged_upto + 1;
                if ((missing_for(next) & (1 << peer.port)) == 0) {
                    peer.blocking_frame = 0;
                } else if (peer.blocking_frame != next) {
                    peer.blocking_frame = next;
                    peer.blocking_since = now;
                } else if (now - peer.blocking_since >= kStuckAfterMs) {
                    drop(peer, kLeft, "its game has not moved on");
                }
                continue;
            }
            int playing = kPlaying;
            if (g_waiting.load() && now - peer.moved_on >= kHostStuckAfterMs &&
                g_stopped.compare_exchange_strong(playing, kHostLeft)) {
                std::fprintf(stderr, "[conker] net: the host's game has not moved on for %lld s: the session is over "
                                     "and the game here has stopped\n", static_cast<long long>(kHostStuckAfterMs / 1000));
                wake_game_thread();
            }
        }
        if (now - last_send >= kSendEveryMs) {
            last_send = now;
            if (is_host()) {
                rejoin_tick(now);
                snapshot_tick();
            } else {
                snapshot_guest_tick(now);
            }
            if (now - last_update >= (g_waiting.load() ? kSendEveryMs : kResendEveryMs)) {
                last_update = now;
                send_updates(true);
            }
        }
        relaunch_to_rejoin();
        if (g_waiting.load()) {
            wake_game_thread();  // so that a long wait notices a drop or a desync
        }
    }
}

// --- connecting ------------------------------------------------------------------------------------

std::filesystem::path data_dir() { return std::filesystem::current_path() / "conker-recomp-data"; }

std::filesystem::path save_name() {
    const std::u8string id = conker::kGameId;
    return std::filesystem::path(id + u8".bin");
}

std::vector<uint8_t> read_own_eeprom() {
    std::vector<uint8_t> eeprom(kEepromSize, 0);
    std::ifstream file(data_dir() / "saves" / save_name(), std::ios::binary);
    if (file) {
        file.read(reinterpret_cast<char*>(eeprom.data()), static_cast<std::streamsize>(eeprom.size()));
    }
    return eeprom;
}

std::vector<uint8_t> session_blob(int port, uint32_t rejoin_from, bool snapshot) {
    Writer w;
    w.u8(static_cast<uint8_t>(port));
    w.u32(rejoin_from);
    w.u8(snapshot ? 1 : 0);
    w.u8(g_ports);
    w.u8(g_start_ports);
    w.u8(g_drop_reason[port] == kNew ? 1 : 0);  // a newcomer, not a player coming back
    w.u8(g_wide ? 1 : 0);
    w.text(g_settings, 4000);
    w.bytes(g_eeprom.data(), g_eeprom.size());
    return std::vector<uint8_t>(w.data, w.data + w.size);
}

bool open_socket(int family, uint16_t bind_port) {
    g_socket = static_cast<int>(socket(family, SOCK_DGRAM, 0));
    if (g_socket < 0) {
        std::fprintf(stderr, "[conker] net: socket: %s\n", socket_error_text().c_str());
        return false;
    }
    socket_prepare(g_socket);
    if (bind_port == 0) {
        return true;
    }
    if (family == AF_INET6) {
        socket_set_option(g_socket, IPPROTO_IPV6, IPV6_V6ONLY, 0);
        sockaddr_in6 addr{};
        addr.sin6_family = AF_INET6;
        addr.sin6_addr = in6addr_any;
        addr.sin6_port = htons(bind_port);
        if (bind(g_socket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            return true;
        }
    } else {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(bind_port);
        if (bind(g_socket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            return true;
        }
    }
    std::fprintf(stderr, "[conker] net: cannot use UDP port %u: %s\n", bind_port, socket_error_text().c_str());
    return false;
}

// A stop signal (SIGTERM, Ctrl-C, the terminal closing) while connecting, before SDL is up to turn it
// into a quit: exit normally then, so atexit work (the router's port mapping removed) still runs.
volatile std::sig_atomic_t g_stop_signal = 0;

// The next datagram, waiting up to timeout_ms for one (0 when none came). One thread at a time: the
// main thread while connecting, then the network thread.
ssize_t receive(uint8_t* buffer, size_t size, sockaddr_storage* from, socklen_t* from_len, int timeout_ms) {
    const int64_t until = now_ms() + timeout_ms;
    for (;;) {
        if (g_stop_signal != 0) {
            std::printf("[conker] net: stopped (signal %d) while connecting\n", static_cast<int>(g_stop_signal));
            std::exit(128 + g_stop_signal);
        }
        int64_t wait = std::max<int64_t>(until - now_ms(), 0);
        const int64_t out_due = sim_pump();
        if (out_due >= 0) {
            wait = std::min(wait, out_due);
        }
        if (g_sim.on) {
            const int64_t now = now_ms();
            for (size_t i = 0; i < g_sim_in.size(); i++) {
                if (g_sim_in[i].due <= now) {
                    Delayed d = std::move(g_sim_in[i]);
                    g_sim_in.erase(g_sim_in.begin() + static_cast<std::ptrdiff_t>(i));
                    const size_t n = std::min(size, d.data.size());
                    std::memcpy(buffer, d.data.data(), n);
                    *from = d.addr;
                    *from_len = d.addr_len;
                    return static_cast<ssize_t>(n);
                }
                wait = std::min(wait, g_sim_in[i].due - now);
            }
        }
        pollfd fd = socket_poll_in(g_socket);
        const bool ready = socket_poll(&fd, 1, static_cast<int>(wait)) > 0;
        if (ready) {
            *from_len = sizeof(*from);
            const ssize_t got = socket_receive_from(g_socket, buffer, size, reinterpret_cast<sockaddr*>(from), from_len);
            // Outside the LAN, with internet play off: dropped before anything looks at it or answers.
            if (got > 0 && !conker::net::sender_allowed(*from)) {
                continue;
            }
            if (got > 0 && !g_sim.on) {
                return got;
            }
            if (got > 0) {
                const int64_t now = now_ms();
                std::lock_guard lock(g_sim_mutex);
                if (!sim_outage(now)) {
                    g_sim_in.push_back({now + sim_delay(), *from, *from_len, std::vector<uint8_t>(buffer, buffer + got)});
                }
            }
            continue;
        }
        if (now_ms() >= until) {
            return 0;
        }
    }
}

// The mods this run loaded, as "+mods-<hash>" ("" for none). A session started in place keeps the mods
// the game started with (only one from the command line starts without any), and a mod on one machine
// and not the others puts them out of sync: so they are part of the build the handshake compares. The
// mods on when the lobby first asks (switching one on the MODS page applies only after a restart, so
// this can be wrong only until then).
std::string mods_text() {
    static const std::string text = [] {
        if (g_active.load()) {
            return std::string();  // from the command line: no mods loaded (game.cpp)
        }
        std::vector<std::string> on;
        for (const recomp::mods::ModDetails& details : recomp::mods::get_all_mod_details("conker")) {
            if (recomp::mods::is_mod_enabled(details.mod_id) || recomp::mods::is_mod_auto_enabled(details.mod_id)) {
                on.push_back(details.mod_id + "@" + details.version.to_string());
            }
        }
        if (on.empty()) {
            return std::string();
        }
        std::sort(on.begin(), on.end());
        uint64_t hash = 0xCBF29CE484222325ull;
        for (const std::string& mod : on) {
            for (const char c : mod + ";") {
                hash = (hash ^ static_cast<uint8_t>(c)) * 0x100000001B3ull;
            }
        }
        char suffix[32];
        std::snprintf(suffix, sizeof(suffix), "+mods-%08x", static_cast<uint32_t>(hash ^ (hash >> 32)));
        return std::string(suffix);
    }();
    return text;
}

// CONKER_NET_TEST_BUILD=<text> poses as another build, to see the handshake and the lobby refuse it.
std::string build_text() {
    const char* test = std::getenv("CONKER_NET_TEST_BUILD");
    return test != nullptr ? std::string(test) : std::string(CONKER_BUILD_VERSION) + mods_text();
}
std::string os_text() { return std::string(kOs) + "-" + kCpu; }
// CONKER_NET_TEST_ROM=<hex> poses as another ROM, the same way.
uint64_t rom_hash() {
    static const uint64_t hash = [] {
        const char* test = std::getenv("CONKER_NET_TEST_ROM");
        return test != nullptr ? std::strtoull(test, nullptr, 16) : conker::kUsRomHash;
    }();
    return hash;
}

// What the host tells the lobby's PROBE (the LAN list and JOIN): a session, how many play, and
// whether a newcomer could join (a port free, or a dropped player's). Kept up to date by the
// network thread, since PROBEs are answered on other threads too.
std::atomic<int> g_offer_players{1};
std::atomic<bool> g_offer_joinable{true};

conker::net::lobby::Offer session_offer() {
    conker::net::lobby::Offer offer;
    offer.name = g_options.name;
    offer.port = g_options.port;
    offer.playing = true;
    offer.players = g_offer_players.load();
    offer.joinable = g_offer_joinable.load();
    // An id of its own: the session id is not handed to whoever asks (it was the one thing a forger of a
    // peer's address lacked before the secrets).
    static const uint32_t id = static_cast<uint32_t>(conker::net::random64()) | 1;
    offer.id = id;
    return offer;
}

// The host, under g_mutex (or before the network thread): the offer's numbers.
void update_offer() {
    int players = 1;
    // A lobby's session (one with a token) takes no newcomers: only its own players, coming back, and they
    // come back with the token on their own command line, never through a lobby's JOIN. So it is never
    // joinable to the OFFER's askers, a dropped port or not: a lobby joiner went on to start again into
    // the session only to be turned away there.
    bool joinable = g_options.token == 0 && g_ports != (1 << kPorts) - 1;
    for (const Peer& peer : g_peers) {
        players += peer.dropped ? 0 : 1;
        joinable = joinable || (g_options.token == 0 && peer.dropped && g_desyncs[peer.port] < kMaxDesyncs &&
                                g_drops[peer.port] < kMaxDrops);
    }
    g_offer_players.store(players);
    g_offer_joinable.store(joinable);
}

bool host_connect() {
    if (!open_socket(AF_INET6, g_options.port) && !open_socket(AF_INET, g_options.port)) {
        return false;
    }
    g_host_key = conker::net::keys::host_key();
    const bool from_lobby = std::getenv("CONKER_NET_TOKEN") != nullptr;  // the lobby's START hands its token so
    std::random_device random;
    g_session = random() | 1;
    g_local_port = 0;
    g_eeprom = read_own_eeprom();
    // From the command line the window does not exist yet, so the widescreen switch is read for the
    // mode it will open in; in place, the window's.
    g_wide = conker::display::widescreen(g_in_place ? conker::display::current() : conker::display::startup_mode());
    g_settings = conker::frame_input::settings_header();
    // In place, the fixed step comes on as the session starts, after this header is made.
    const std::string fixed_text = "fixed_step=0";
    if (const size_t at = g_settings.find(fixed_text); g_in_place && at != std::string::npos) {
        g_settings[at + fixed_text.size() - 1] = '1';
    }
    const std::string wide_line = "# switch WIDESCREEN=";
    if (const size_t at = g_settings.find(wide_line); at != std::string::npos) {
        g_settings[at + wide_line.size()] = g_wide ? '1' : '0';
    }
    g_ports = static_cast<uint8_t>((1 << g_options.players) - 1);
    g_start_ports = g_ports;
    std::printf("[conker] net: hosting on UDP port %u for %d players; waiting for %d to join\n", g_options.port,
                g_options.players, g_options.players - 1);
    std::printf("[conker] net: key check %s\n", conker::net::keys::key_check(g_host_key.pub).c_str());
    if (!from_lobby && g_options.token != 0) {
        // Started by hand: how to join it. The token is this session's own; the log stays here.
        std::printf("[conker] net: guests join with --net-join <this machine's address>:%u --net-token %016llx "
                    "--net-host-key %s\n", g_options.port, static_cast<unsigned long long>(g_options.token),
                    seal::hex(g_host_key.pub).c_str());
    } else if (g_options.open) {
        std::printf("[conker] net: --net-open: anyone on the network with this build can join\n");
    }
    if (g_options.map && conker::net::internet_allowed()) {
        conker::net::portmap::open(g_options.port);
    }
    conker::net::lobby::serve_discovery(session_offer);
    std::vector<std::vector<uint8_t>> blobs;
    int64_t last_send = 0;
    int64_t last_note = now_ms();
    const int64_t host_started = last_note;
    uint8_t buffer[kMaxPacket + 64];
    for (;;) {
        if (g_net_stop.load()) {
            return false;
        }
        // In place, the lobby's players come within seconds of its START, or not at all.
        if (g_in_place && now_ms() - host_started >= kInPlaceConnectMs) {
            std::fprintf(stderr, "[conker] net: not every player came within %lld s of the start\n",
                         static_cast<long long>(kInPlaceConnectMs / 1000));
            set_place_message("Not every player came within " + std::to_string(kInPlaceConnectMs / 1000) +
                              " s of the start: their game may have been closed, or the network lost.");
            return false;
        }
        sockaddr_storage from{};
        socklen_t from_len = 0;
        const ssize_t got = receive(buffer, sizeof(buffer), &from, &from_len, 20);
        const int64_t now = now_ms();
        if (got > 0) {
            if (conker::net::lobby::answer_probe(g_socket, buffer, static_cast<size_t>(got), from, from_len,
                                                 session_offer())) {
                continue;
            }
            uint8_t plain[kMaxPacket + 64];
            size_t plain_size = 0;
            bool contact = false;
            if (!host_open(buffer, static_cast<size_t>(got), from, from_len, plain, sizeof(plain), &plain_size, &contact)) {
                continue;
            }
            Reader r{plain, plain_size};
            const bool magic = r.u32() == kMagic;
            const uint8_t type = r.u8();
            const uint32_t session = r.u32();
            const uint64_t secret = r.u64();
            Peer* peer = contact ? nullptr : peer_from(from, from_len);
            if (magic && r.ok && type == kHello && contact) {
                const std::string build = r.text(200);
                const std::string os = r.text(64);
                const uint64_t rom = r.u64();
                const int wanted = r.u8() - 1;  // the port the lobby gave it (0 for any)
                r.u8();  // whether it wants no snapshot
                const uint64_t cookie = r.u64();
                const uint64_t token = r.u64();
                if (!r.ok || secret == 0) {
                    continue;
                }
                if (!conker::net::address_cookie_ok(from, kCookiePurpose, cookie)) {
                    Writer w;
                    begin(w, kCookie, secret);
                    w.u64(conker::net::address_cookie(from, kCookiePurpose));
                    send_contact(from, from_len, w);
                    continue;
                }
                std::string problem;
                if (build != build_text()) {
                    problem = "the host runs build " + build_text() + ", this is " + conker::net::printable(build, 40);
                } else if (rom != conker::net::rom_hash()) {
                    problem = "the host plays another ROM";
                } else if (g_options.token != 0 && !conker::net::same_secret(token, g_options.token)) {
                    problem = "this session lets in only its own players (its lobby's, or those given its --net-token)";
                } else if (static_cast<int>(g_peers.size()) + 1 >= g_options.players) {
                    problem = "the session is full";
                }
                if (!problem.empty()) {
                    if (conker::net::log_refusal()) {
                        std::printf("[conker] net: %s turned away: %s\n", address_text(from, from_len).c_str(), problem.c_str());
                    }
                    Writer w;
                    begin(w, kReject, secret);
                    w.text(problem, 200);
                    send_contact(from, from_len, w);
                    continue;
                }
                // The port the lobby gave it, if free; else the first free one.
                auto taken = [](int port) {
                    return std::any_of(g_peers.begin(), g_peers.end(), [port](const Peer& p) { return p.port == port; });
                };
                int port = wanted >= 1 && wanted < g_options.players && !taken(wanted) ? wanted : 1;
                while (taken(port)) {
                    port++;
                }
                Peer joined;
                joined.addr = from;
                joined.addr_len = from_len;
                joined.port = port;
                joined.secret = secret;
                joined.channel = g_contact;
                joined.last_heard = now;
                g_peers.push_back(joined);
                // In place, the guest's game becomes the host's from a snapshot, so its port is held with
                // nothing pressed from frame 1 until it has (host_in_place_start).
                blobs.push_back(g_in_place ? session_blob(joined.port, 1, true) : session_blob(joined.port, 0, false));
                std::printf("[conker] net: player %d joined from %s (%s)\n", joined.port + 1,
                            address_text(from, from_len).c_str(), conker::net::printable(os, 40).c_str());
                g_offer_players.store(static_cast<int>(g_peers.size()) + 1);
                g_offer_joinable.store(static_cast<int>(g_peers.size()) + 1 < g_options.players);
                last_send = 0;
            } else if (magic && r.ok && peer != nullptr && session == g_session && conker::net::same_secret(secret, peer->secret)) {
                peer->last_heard = now;
                if (type == kReady) {
                    const uint32_t sent = r.u32();
                    if (r.ok && !peer->ready) {
                        peer->ready = true;
                        peer->rtt = rtt_from_echo(sent);
                        std::printf("[conker] net: player %d ready (round trip %d ms)\n", peer->port + 1, peer->rtt);
                    }
                } else if (type == kStarted && !peer->started) {
                    peer->started = true;
                }
            }
        }
        // A guest that has gone quiet gives its place back: one not ready yet soon, a ready one (which
        // says READY again while it waits) after kDropAfterMs.
        for (size_t i = 0; i < g_peers.size();) {
            const Peer& p = g_peers[i];
            if (now - p.last_heard > (p.ready ? kDropAfterMs : kUnreadyGoneMs)) {
                std::printf("[conker] net: player %d (%s) went quiet before the start; the place is free again\n",
                            p.port + 1, address_text(p.addr, p.addr_len).c_str());
                g_peers.erase(g_peers.begin() + static_cast<std::ptrdiff_t>(i));
                blobs.erase(blobs.begin() + static_cast<std::ptrdiff_t>(i));
                g_offer_players.store(static_cast<int>(g_peers.size()) + 1);
                g_offer_joinable.store(static_cast<int>(g_peers.size()) + 1 < g_options.players);
            } else {
                i++;
            }
        }
        const bool all_ready = static_cast<int>(g_peers.size()) + 1 == g_options.players &&
                               std::all_of(g_peers.begin(), g_peers.end(), [](const Peer& p) { return p.ready; });
        if (all_ready && g_options.delay < 0) {
            int rtt = 0;
            for (const Peer& peer : g_peers) {
                rtt = std::max(rtt, peer.rtt);
            }
            // A guest's input reaches the host and comes back in the merged record: a whole round trip,
            // plus the 10 ms send tick and a little frame timing. D = ceil((RTT + 15 ms) / 33.3 ms),
            // at least 2. Half the round trip (the plan's first rule) held 150 ms play to 27 fps.
            g_options.delay = std::clamp(static_cast<int>(std::ceil((rtt + 15) / 33.3)), 2, kMaxDelay);
        }
        if (all_ready && std::all_of(g_peers.begin(), g_peers.end(), [](const Peer& p) { return p.started; })) {
            break;
        }
        if (now - last_send >= 100) {
            last_send = now;
            for (size_t i = 0; i < g_peers.size(); i++) {
                Peer& peer = g_peers[i];
                if (!peer.ready) {
                    const std::vector<uint8_t>& blob = blobs[i];
                    for (size_t offset = 0; offset < blob.size(); offset += kChunkSize) {
                        Writer w;
                        begin(w, kChunk, peer.secret);
                        w.u32(static_cast<uint32_t>(now));
                        w.u32(static_cast<uint32_t>(blob.size()));
                        w.u32(static_cast<uint32_t>(offset));
                        const size_t length = std::min(kChunkSize, blob.size() - offset);
                        w.u16(static_cast<uint16_t>(length));
                        w.bytes(blob.data() + offset, length);
                        send_to(peer, w);
                    }
                } else if (all_ready && !peer.started) {
                    Writer w;
                    begin(w, kStart, peer.secret);
                    w.u8(static_cast<uint8_t>(g_options.delay));
                    send_to(peer, w);
                }
            }
        }
        if (now - last_note >= 10000 && !all_ready) {
            last_note = now;
            std::printf("[conker] net: %zu of %d players here; waiting\n", g_peers.size() + 1, g_options.players);
        }
    }
    g_delay = g_options.delay;
    update_offer();
    std::printf("[conker] net: every player is here; input delay %d frames\n", g_delay);
    return true;
}

bool resolve(const std::string& text, sockaddr_storage* addr, socklen_t* len) {
    std::string host = text;
    std::string port = std::to_string(conker::net::kDefaultPort);
    if (!host.empty() && host[0] == '[') {
        const size_t close = host.find(']');
        if (close == std::string::npos) {
            return false;
        }
        if (close + 1 < host.size() && host[close + 1] == ':') {
            port = host.substr(close + 2);
        }
        host = host.substr(1, close - 1);
    } else if (std::count(host.begin(), host.end(), ':') == 1) {
        const size_t colon = host.find(':');
        port = host.substr(colon + 1);
        host = host.substr(0, colon);
    }
    addrinfo hints{};
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_family = AF_UNSPEC;
    addrinfo* found = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &found) != 0 || found == nullptr) {
        return false;
    }
    std::memcpy(addr, found->ai_addr, found->ai_addrlen);
    *len = static_cast<socklen_t>(found->ai_addrlen);
    freeaddrinfo(found);
    return true;
}

// A guest's secret for joining `address`: the same each time this machine (its data folder) joins
// that host's session, so that a guest that quit or was dropped and starts again gets its own port back
// (conker-recomp-data/net/secrets.txt, "address secret" lines; CONKER_NET_REJOIN_SECRET when it starts
// itself again to rejoin). A lobby's session is its own (the address and the session's token): one
// secret for every session at an address made a secret seen once good for all of them.
uint64_t rejoin_secret(const std::string& host_address) {
    std::string address = host_address;
    if (g_options.token != 0) {
        char token[18];
        std::snprintf(token, sizeof(token), "#%016llx", static_cast<unsigned long long>(g_options.token));
        address += token;
    }
    if (g_rejoin_secret_kept != 0) {
        return g_rejoin_secret_kept;
    }
    if (const char* env = std::getenv("CONKER_NET_REJOIN_SECRET")) {
        if (const uint64_t value = std::strtoull(env, nullptr, 16); value != 0) {
            return value;
        }
    }
    const std::filesystem::path file = data_dir() / "net" / "secrets.txt";
    std::vector<std::string> lines;
    {
        std::ifstream in(file);
        std::string line;
        while (std::getline(in, line)) {
            const size_t space = line.find(' ');
            if (space != std::string::npos && line.substr(0, space) == address) {
                if (const uint64_t value = std::strtoull(line.c_str() + space + 1, nullptr, 16); value != 0) {
                    return value;
                }
                continue;
            }
            lines.push_back(line);
        }
    }
    const uint64_t secret = conker::net::random64();
    char text[17];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(secret));
    lines.push_back(address + " " + text);
    while (lines.size() > 32) {
        lines.erase(lines.begin());
    }
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    std::ofstream out(file, std::ios::trunc);
    for (const std::string& line : lines) {
        out << line << "\n";
    }
    return secret;
}

bool guest_connect() {
    g_join_start = now_ms();
    Peer host;
    if (!resolve(g_options.address, &host.addr, &host.addr_len)) {
        std::fprintf(stderr, "[conker] net: cannot find the host %s\n", g_options.address.c_str());
        return false;
    }
    if (!conker::net::internet_allowed() && !conker::net::lan_address(host.addr)) {
        std::fprintf(stderr, "[conker] net: cannot join %s: %s\n", address_text(host.addr, host.addr_len).c_str(),
                     conker::net::internet_off_message().c_str());
        return false;
    }
    if (!open_socket(host.addr.ss_family, 0)) {
        return false;
    }
    g_secret = rejoin_secret(g_options.address);
    g_guest_key = seal::new_key_pair();
    std::printf("[conker] net: joining %s\n", address_text(host.addr, host.addr_len).c_str());
    bool keyed = false;           // the host's key is in: host.channel is the link with it
    uint8_t host_pub[32] = {};
    bool told_other_key = false;
    std::vector<uint8_t> blob;
    std::vector<bool> have;
    size_t have_bytes = 0;
    bool ready = false;
    uint64_t cookie = 0;  // the host's COOKIE for this machine's address, sent back in every HELLO
    int64_t last_hello = 0;
    int64_t last_ready = 0;
    const int64_t started = now_ms();
    int64_t last_note = started;
    uint8_t buffer[kMaxPacket + 64];
    for (;;) {
        const int64_t now = now_ms();
        if (g_net_stop.load()) {
            return false;
        }
        if (g_in_place && now - started >= kInPlaceConnectMs) {
            std::fprintf(stderr, "[conker] net: the host's session did not start within %lld s\n",
                         static_cast<long long>(kInPlaceConnectMs / 1000));
            set_place_message("The host's session did not start within " + std::to_string(kInPlaceConnectMs / 1000) +
                              " s. Its game may have been closed, or a firewall blocks the game.");
            return false;
        }
        if (blob.empty() && now - last_hello >= 250 && !keyed) {
            // The host's key first (net_seal.hpp).
            last_hello = now;
            uint8_t ask[seal::kAskSize];
            send_datagram(host.addr, host.addr_len, ask, seal::write_ask(g_guest_key, ask, sizeof(ask)));
        } else if (blob.empty() && now - last_hello >= 250) {
            last_hello = now;
            Writer w;
            begin(w, kHello, g_secret);  // the session id is 0 until the host's first CHUNK
            w.text(build_text(), 200);
            w.text(os_text(), 64);
            w.u64(conker::net::rom_hash());
            const char* rejoin = std::getenv("CONKER_NET_REJOIN_PORT");
            w.u8(static_cast<uint8_t>(g_rejoin_port != 0 ? g_rejoin_port
                                      : rejoin != nullptr ? std::atoi(rejoin) : g_options.want_port));
            // Replay, not a snapshot (in place there is no replay: the game here is not the session's).
            w.u8(!g_in_place && std::getenv("CONKER_NET_NO_SNAPSHOT") != nullptr ? 1 : 0);
            w.u64(cookie);
            w.u64(g_options.token);
            uint8_t out[sizeof(w.data) + seal::kFirstOverhead];
            const size_t size = seal::seal_first(host.channel, cookie, w.data, w.size, out, sizeof(out));
            send_datagram(host.addr, host.addr_len, out, size);
        }
        if (now - last_note >= 10000) {
            last_note = now;
            std::printf("[conker] net: %s, waiting for the host\n", ready ? "ready" : "no answer yet");
        }
        if (ready && now - last_ready >= kReadyAgainMs) {
            // Still here, while the host waits for the others (it lets a quiet guest's place go).
            last_ready = now;
            Writer w;
            begin(w, kReady, g_secret);
            w.u32(static_cast<uint32_t>(now));
            send_to(host, w);
        }
        sockaddr_storage from{};
        socklen_t from_len = 0;
        const ssize_t got = receive(buffer, sizeof(buffer), &from, &from_len, 20);
        if (got <= 0 || !same_address(from, from_len, host.addr, host.addr_len)) {
            continue;
        }
        uint64_t offered_cookie = 0;
        uint8_t offered_key[32];
        if (seal::read_offer(buffer, static_cast<size_t>(got), &offered_cookie, offered_key)) {
            if (!g_expected.matches(offered_key)) {
                if (!told_other_key) {
                    told_other_key = true;
                    std::fprintf(stderr, "[conker] net: %s answered with another key than the host's (%s): a machine "
                                 "posing as the host? Not joining it; waiting for the host\n",
                                 address_text(host.addr, host.addr_len).c_str(),
                                 g_expected.has_key ? "the one this machine was given" : "its join code's");
                }
                continue;
            }
            // With nothing to go on (an address on the command line), the key this machine saw there last.
            uint8_t known[32];
            if (!keyed && !g_expected.has_key && !g_expected.has_fingerprint &&
                conker::net::keys::known_host(g_options.address, known) && std::memcmp(known, offered_key, 32) != 0) {
                set_place_message("The host's key has changed since this machine last joined it: another machine "
                                  "posing as the host?");
                std::fprintf(stderr, "[conker] net: %s's key has changed since this machine last joined it (key check "
                             "%s, was %s): another machine posing as the host? Not joining it. If the host shows key "
                             "check %s, join with --net-host-key %s\n", g_options.address.c_str(),
                             conker::net::keys::key_check(offered_key).c_str(), conker::net::keys::key_check(known).c_str(),
                             conker::net::keys::key_check(offered_key).c_str(), seal::hex(offered_key).c_str());
                return false;
            }
            if (!keyed) {
                keyed = true;
                std::memcpy(host_pub, offered_key, sizeof(host_pub));
                host.channel = seal::guest_channel(g_guest_key, host_pub);
                last_hello = 0;  // the HELLO at once
            }
            // The first key offered is the host's from then on; a new cookie for it is taken.
            if (std::memcmp(host_pub, offered_key, sizeof(host_pub)) == 0 && blob.empty() && offered_cookie != cookie) {
                cookie = offered_cookie;
                last_hello = 0;
            }
            continue;
        }
        uint8_t plain[kMaxPacket + 64];
        size_t plain_size = 0;
        if (!keyed || !seal::open(host.channel, buffer, static_cast<size_t>(got), plain, sizeof(plain), &plain_size)) {
            continue;
        }
        Reader r{plain, plain_size};
        if (r.u32() != kMagic) {
            continue;
        }
        const uint8_t type = r.u8();
        const uint32_t session = r.u32();
        if (!r.ok || !conker::net::same_secret(r.u64(), g_secret)) {
            continue;
        }
        if (type == kReject) {
            const std::string why = conker::net::printable(r.text(200), 200);
            std::fprintf(stderr, "[conker] net: the host turned this machine away: %s\n", why.c_str());
            set_place_message("The host turned this machine away: " + why);
            return false;
        }
        if (type == kCookie) {
            const uint64_t value = r.u64();
            if (r.ok && blob.empty() && value != cookie) {
                cookie = value;
                last_hello = 0;  // the HELLO again at once, with it
            }
            continue;
        }
        if (type == kChunk) {
            const uint32_t sent = r.u32();
            const uint32_t total = r.u32();
            const uint32_t offset = r.u32();
            const uint16_t length = r.u16();
            if (!r.ok || !chunk_fits(total, 64 * 1024, offset, length, kChunkSize) ||
                r.at + length > r.size) {
                continue;
            }
            if (blob.empty()) {
                g_session = session;
                blob.assign(total, 0);
                have.assign((total + kChunkSize - 1) / kChunkSize, false);
            }
            if (session != g_session || total != blob.size() || offset % kChunkSize != 0) {
                continue;
            }
            if (!have[offset / kChunkSize]) {
                have[offset / kChunkSize] = true;
                std::memcpy(blob.data() + offset, r.data + r.at, length);
                have_bytes += length;
            }
            if (have_bytes == blob.size()) {
                if (!ready) {
                    ready = true;
                    Reader b{blob.data(), blob.size()};
                    g_local_port = b.u8();
                    g_rejoin_from = b.u32();
                    g_snap_coming = b.u8() != 0;
                    g_ports = b.u8();
                    g_start_ports = b.u8();
                    const bool newcomer = b.u8() != 0;
                    g_wide = b.u8() != 0;
                    g_settings = b.text(4000);
                    g_eeprom.assign(kEepromSize, 0);
                    for (uint8_t& byte : g_eeprom) {
                        byte = b.u8();
                    }
                    if (!b.ok || g_local_port < 1 || g_local_port >= kPorts || (g_ports & (1 << g_local_port)) == 0) {
                        std::fprintf(stderr, "[conker] net: the host's session could not be read\n");
                        set_place_message("The host's session could not be read.");
                        return false;
                    }
                    // In place the game here is not the session's (nor a fresh boot to replay it from):
                    // only the host's snapshot makes it so.
                    if (g_in_place && (!g_snap_coming || g_rejoin_from == 0)) {
                        std::fprintf(stderr, "[conker] net: the host sends no snapshot of its game; cannot join in place\n");
                        set_place_message("The host could not send its game to this machine.");
                        return false;
                    }
                    if (g_in_place && g_rejoin_from == 1 && g_rejoin_port == 0) {
                        std::printf("[conker] net: in the session as player %d; the host's game comes as a snapshot\n",
                                    g_local_port + 1);
                    } else if (g_rejoin_from != 0 && newcomer) {
                        std::printf("[conker] net: joining the running session as player %d (its port held from "
                                    "frame %u; plugged in outside a match); catching up\n", g_local_port + 1,
                                    g_rejoin_from);
                    } else if (g_rejoin_from != 0) {
                        std::printf("[conker] net: rejoining the session as player %d (dropped from frame %u); "
                                    "catching up\n", g_local_port + 1, g_rejoin_from);
                    } else {
                        std::printf("[conker] net: in the session as player %d\n", g_local_port + 1);
                    }
                    conker::net::keys::remember_host(g_options.address, host_pub);
                }
                Writer w;
                begin(w, kReady, g_secret);
                w.u32(sent);
                send_to(host, w);
                last_ready = now;
            }
            continue;
        }
        if (type == kStart && ready && session == g_session) {
            const uint8_t delay = r.u8();
            if (!r.ok || delay < 1 || delay > kMaxDelay) {
                continue;
            }
            g_delay = delay;
            Writer w;
            begin(w, kStarted, g_secret);
            send_to(host, w);
            break;
        }
    }
    host.last_heard = now_ms();
    host.port = 0;
    g_peers.push_back(host);
    std::printf("[conker] net: the game starts; input delay %d frames\n", g_delay);
    return true;
}

// --- the corner text -------------------------------------------------------------------------------

// 5x7 glyphs, a row a byte (bit 4 the left column).
struct Glyph {
    char c;
    uint8_t rows[7];
};
constexpr Glyph kFont[] = {
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}}, {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}}, {'3', {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}}, {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}}, {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}}, {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}}, {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}}, {'D', {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}}, {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}}, {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}}, {'J', {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}}, {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}}, {'N', {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}}, {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}}, {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}}, {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}}, {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}}, {'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},
    {'Y', {0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04}}, {'Z', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}},
    {':', {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00}}, {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}},
    {'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}}, {'/', {0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10}},
};

const Glyph* glyph(char c) {
    for (const Glyph& g : kFont) {
        if (g.c == c) {
            return &g;
        }
    }
    return nullptr;
}

// The display list the renderer runs in place of the game's (two of them, used in turn, so that the
// one RT64 may still be reading is not rewritten), room for the lobby's session page too: a
// rectangle for each run of lit pixels in a glyph's row. The renderer's "my view" copy of RDRAM
// takes the scratch memory from here up (renderer_rt64.cpp's draw_from): keep them above it.
constexpr uint32_t kOverlayLists[] = {0x807D0000, 0x807D8000};
constexpr uint32_t kOverlaySize = 0x8000;
constexpr int kTextX = 12;
constexpr int kTextY = 214;  // the bottom left, clear of the HUD

struct ListWriter {
    uint8_t* rdram;
    uint32_t at;
    uint32_t end;
    bool full = false;
    void command(uint32_t w0, uint32_t w1) {
        if (at + 16 > end) {  // room for G_ENDDL stays
            full = true;
            return;
        }
        MEM_W(0, kseg0(at)) = static_cast<int32_t>(w0);
        MEM_W(4, kseg0(at)) = static_cast<int32_t>(w1);
        at += 8;
    }
    // A one-cycle G_FILLRECT, from (x, y) to before (x + w, y + h).
    void rect(int x, int y, int w, int h) {
        const uint32_t lrx = static_cast<uint32_t>(x + w) << 2;
        const uint32_t lry = static_cast<uint32_t>(y + h) << 2;
        command(0xF6000000u | (lrx << 12) | lry, (static_cast<uint32_t>(x) << 14) | (static_cast<uint32_t>(y) << 2));
    }
};

// The colour image the game's display list leaves set at its end, found by walking it (calls,
// branches and segments followed): {address, width, size}, or an empty one. The text is drawn into
// it, and RT64 writes it back into RDRAM, so it must be a frame buffer: at boot and in some frames
// the list ends on another image, a smaller one in the game's heap.
struct ColorImage {
    uint32_t address = 0;
    uint32_t width = 0;
    uint32_t size = 0;
};

ColorImage last_color_image(const uint8_t* rdram, uint32_t list) {
    constexpr uint32_t kRdramSize = 0x800000;
    constexpr int kMaxDepth = 16;
    constexpr int kMaxCommands = 400000;
    uint32_t segments[16] = {};
    uint32_t stack[kMaxDepth];
    int depth = 0;
    ColorImage image;
    uint32_t pc = list & 0x00FFFFFF;
    const auto read = [&](uint32_t at) {
        uint32_t value;
        std::memcpy(&value, rdram + at, 4);  // rdram holds native-endian 32-bit words
        return value;
    };
    const auto resolve = [&](uint32_t addr) { return (segments[(addr >> 24) & 0xF] + (addr & 0x00FFFFFF)) & 0x00FFFFFF; };
    for (int count = 0; count < kMaxCommands && pc + 8 <= kRdramSize; count++) {
        const uint32_t w0 = read(pc);
        const uint32_t w1 = read(pc + 4);
        pc += 8;
        switch (w0 >> 24) {
            case 0xDB:  // G_MOVEWORD: a segment
                if (((w0 >> 16) & 0xFF) == 0x06) {
                    segments[((w0 & 0xFFFF) / 4) & 0xF] = w1 & 0x00FFFFFF;
                }
                break;
            case 0xDE:  // G_DL: a call, or a branch
                if (((w0 >> 16) & 0xFF) == 0) {
                    if (depth >= kMaxDepth) {
                        return {};
                    }
                    stack[depth++] = pc;
                }
                pc = resolve(w1);
                break;
            case 0xDF:  // G_ENDDL
                if (depth == 0) {
                    return image;
                }
                pc = stack[--depth];
                break;
            case 0xFF:  // G_SETCIMG
                image = {resolve(w1), (w0 & 0xFFF) + 1, (w0 >> 19) & 3};
                break;
            default:
                break;
        }
    }
    return {};
}

void draw_text(ListWriter& list, const std::string& text, int x0, int y0, int scale = 1) {
    int x = x0;
    for (const char c : text) {
        const Glyph* g = glyph(c);
        if (g != nullptr) {
            for (int row = 0; row < 7; row++) {
                // Runs of lit pixels in the row, one rectangle each.
                int col = 0;
                while (col < 5) {
                    if ((g->rows[row] & (0x10 >> col)) == 0) {
                        col++;
                        continue;
                    }
                    int run = 1;
                    while (col + run < 5 && (g->rows[row] & (0x10 >> (col + run))) != 0) {
                        run++;
                    }
                    list.rect(x + col * scale, y0 + row * scale, run * scale, scale);
                    col += run;
                }
            }
        }
        x += 6 * scale;
    }
}

}  // namespace

namespace conker::net {

namespace {

// The --net options in argv into g_options (the others into kept). False, with a message, on a bad one.
bool parse_net_options(int argc, char** argv, std::vector<char*>* kept_out) {
    std::vector<char*>& kept = *kept_out;
    for (int i = 0; i < argc; i++) {
        const std::string arg = argv[i];
        const bool has_value = i + 1 < argc && argv[i + 1][0] != '-';
        if (arg == "--net-host") {
            g_options.host = true;
            if (has_value && std::strchr(argv[i + 1], '.') == nullptr && std::atoi(argv[i + 1]) > 0) {
                g_options.port = static_cast<uint16_t>(std::atoi(argv[++i]));
            }
        } else if (arg == "--net-join") {
            if (!has_value) {
                std::fprintf(stderr, "[conker] --net-join needs <address:port>\n");
                return false;
            }
            g_options.join = true;
            g_options.address = argv[++i];
        } else if (arg == "--net-delay") {
            if (!has_value || std::atoi(argv[i + 1]) < 1 || std::atoi(argv[i + 1]) > kMaxDelay) {
                std::fprintf(stderr, "[conker] --net-delay needs a number of frames from 1 to %d\n", kMaxDelay);
                return false;
            }
            g_options.delay = std::atoi(argv[++i]);
        } else if (arg == "--net-players") {
            if (!has_value || std::atoi(argv[i + 1]) < 2 || std::atoi(argv[i + 1]) > kPorts) {
                std::fprintf(stderr, "[conker] --net-players needs a number from 2 to %d\n", kPorts);
                return false;
            }
            g_options.players = std::atoi(argv[++i]);
        } else if (arg == "--net-want-port") {
            if (!has_value || std::atoi(argv[i + 1]) < 2 || std::atoi(argv[i + 1]) > kPorts) {
                std::fprintf(stderr, "[conker] --net-want-port needs a port from 2 to %d\n", kPorts);
                return false;
            }
            g_options.want_port = std::atoi(argv[++i]);
        } else if (arg == "--net-name") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "[conker] --net-name needs a name\n");
                return false;
            }
            g_options.name = argv[++i];
        } else if (arg == "--net-token") {
            if (!has_value) {
                std::fprintf(stderr, "[conker] --net-token needs the lobby's session token\n");
                return false;
            }
            g_options.token = std::strtoull(argv[++i], nullptr, 16);
        } else if (arg == "--net-open") {
            g_options.open = true;
        } else if (arg == "--net-host-key") {
            if (!has_value || !seal::expected_from_hex(argv[i + 1], &g_expected) || !g_expected.has_key) {
                std::fprintf(stderr, "[conker] --net-host-key needs the host's key (64 hex digits, as the host prints it)\n");
                return false;
            }
            i++;
        } else if (arg == "--net-map") {
            g_options.map = true;
        } else if (arg == "--net-lobby") {
            g_options.lobby = true;
        } else if (arg == "--net-unsafe-internet") {
            g_options.internet = true;
        } else if (arg == "--net-lan-only") {
            g_options.internet = false;
        } else {
            kept.push_back(argv[i]);
        }
    }
    if (g_options.host && g_options.join) {
        std::fprintf(stderr, "[conker] --net-host and --net-join cannot go together\n");
        return false;
    }
    if (const char* env = std::getenv("CONKER_NET_TOKEN"); g_options.token == 0 && env != nullptr) {
        g_options.token = std::strtoull(env, nullptr, 16);  // the lobby's start hands it over this way
    }
    // A host started by hand makes a token of its own (host_connect prints how to join it), unless
    // --net-open: without one, anyone who reached the port took a seat and got the host's save.
    if (g_options.host && g_options.token == 0 && !g_options.open) {
        g_options.token = conker::net::random64();
    }
    if (g_options.open) {
        g_options.token = 0;
    }
    // The host's key as the lobby had it (net_seal.hpp): its own for the host, its public half for a guest.
    if (const char* env = std::getenv("CONKER_NET_HOST_PUBLIC"); g_options.join && env != nullptr && !g_expected.has_key) {
        if (!seal::expected_from_hex(env, &g_expected)) {
            std::fprintf(stderr, "[conker] net: CONKER_NET_HOST_PUBLIC is not a host key or fingerprint\n");
            return false;
        }
    }
    return true;
}

}  // namespace

bool parse_args(int* argc, char** argv) {
    if (!sockets_init()) {
        std::fprintf(stderr, "[conker] net: sockets cannot be used: %s\n", socket_error_text().c_str());
    }
    g_argv.assign(argv, argv + *argc);
    std::vector<char*> kept;
    if (!parse_net_options(*argc, argv, &kept)) {
        return false;
    }
    g_plain_argv.assign(kept.begin(), kept.end());
    for (size_t i = 0; i < kept.size(); i++) {
        argv[i] = kept[i];
    }
    argv[kept.size()] = nullptr;
    *argc = static_cast<int>(kept.size());
    g_active = g_options.host || g_options.join;
    conker::net::set_internet_allowed(g_options.internet);
    if (g_options.internet) {
        std::printf("[conker] net: UNSAFE: internet play on (%s): the online code's security is not trusted yet "
                    "(docs/online-play.md)\n",
                    CONKER_NET_INTERNET_DEFAULT != 0 ? "this build's default; --net-lan-only turns it off"
                                                     : "--net-unsafe-internet");
    }
    return true;
}

bool active() { return g_active; }

uint32_t game_memory_end() { return g_active ? kHashEnd : 0; }

bool lobby_at_boot() { return g_options.lobby; }

int delay_option() { return g_options.delay; }

std::string build_text() { return ::build_text(); }
uint64_t rom_hash() { return ::rom_hash(); }
std::string os_text() { return ::os_text(); }

void relaunch(const std::vector<std::string>& net_args) {
    std::vector<std::string> line = g_plain_argv;
    line.insert(line.end(), net_args.begin(), net_args.end());
    // The session keeps the lobby's choice, whichever way this build starts.
    line.push_back(g_options.internet ? "--net-unsafe-internet" : "--net-lan-only");
    process::start_again(line);
}

void say_goodbye();
// Once: say_goodbye at exit (registered after the threads it stops exist, so it runs before they go).
void register_goodbye() {
    static const bool goodbye = (std::atexit(say_goodbye), true);
    (void)goodbye;
}
void say_goodbye_now();

namespace {

// Connected (either way): the session's first frames, and the network thread.
void finish_connect() {
    for (int port = 0; port < kPorts; port++) {
        g_input_upto[port] = static_cast<uint32_t>(g_delay);
        g_unplugged[port] = (g_start_ports & (1 << port)) == 0;  // nobody's: a newcomer's comes in later
    }
    g_merged_upto = static_cast<uint32_t>(g_delay);
    g_session_start.store(now_ms());
    g_net_stop.store(false);
    g_thread = std::thread(network_thread);
    register_goodbye();
}

// The host of a session started in place: each guest's game becomes the host's from a snapshot, so
// its port is held with nothing pressed from frame 1 (as a newcomer's) until it has played past it.
void host_in_place_start() {
    std::lock_guard lock(g_mutex);
    const int64_t now = now_ms();
    for (Peer& peer : g_peers) {
        peer.dropped = true;
        peer.rejoining = true;
        peer.needs_snapshot = true;
        peer.dropped_at = now;
        peer.last_heard = now;
        g_drop_frame[peer.port] = 1;
        g_drop_reason[peer.port] = kNew;
        g_live_frame[peer.port] = 0;
    }
}

// Every session's state back to how a run starts, for the next session in place.
void reset_session() {
    std::lock_guard lock(g_mutex);
    g_socket = -1;
    g_session = 0;
    g_local_port = 0;
    g_ports = 0;
    g_start_ports = 0;
    g_delay = 2;
    g_settings.clear();
    g_wide = false;
    g_eeprom.clear();
    for (auto& ring : g_inputs) {
        std::fill(std::begin(ring), std::end(ring), Input{});
    }
    std::fill(std::begin(g_input_upto), std::end(g_input_upto), 0u);
    std::fill(std::begin(g_merged), std::end(g_merged), Merged{});
    g_history.clear();
    g_host_upto = 0;
    g_merged_upto = 0;
    std::fill(std::begin(g_own_hashes), std::end(g_own_hashes), Hash{});
    g_own_hash_upto = 0;
    g_own_hash_history.clear();
    g_own_hash_base = 0;
    for (auto& ring : g_peer_hashes) {
        std::fill(std::begin(ring), std::end(ring), Hash{});
    }
    std::fill(std::begin(g_peer_played), std::end(g_peer_played), 0u);
    g_peers.clear();
    std::fill(std::begin(g_drop_frame), std::end(g_drop_frame), 0u);
    std::fill(std::begin(g_drop_reason), std::end(g_drop_reason), uint8_t{0});
    std::fill(std::begin(g_live_frame), std::end(g_live_frame), 0u);
    std::fill(std::begin(g_desyncs), std::end(g_desyncs), 0);
    std::fill(std::begin(g_drops), std::end(g_drops), 0);
    g_rejoin_from = 0;
    std::fill(std::begin(g_snap_failed), std::end(g_snap_failed), false);
    g_snap_requested.store(false);
    g_snap_blob = nullptr;
    g_snap_frame = 0;
    g_snap_serial = 0;
    g_snap_coming = false;
    g_snap_tried = false;
    g_snap_data.clear();
    g_snap_have.clear();
    g_snap_have_count = 0;
    g_snap_of = 0;
    g_snap_waiting.store(false);
    g_join_start = 0;
    g_live_announced = false;
    g_catching_up.store(false);
    g_stopped.store(kPlaying);
    std::fill(std::begin(g_unplugged), std::end(g_unplugged), false);
    g_stopped_frame = 0;
    g_host_waiting_for = 0;
    g_waiting.store(false);
    g_stall_seen.store(0);
    g_stall_port.store(-1);
    g_hashes_sent_upto = 0;
    for (Detail& detail : g_details) {
        detail = Detail{};
    }
    g_relaunch_started = false;
    g_secret = 0;
    g_session_start.store(0);
    g_catch_up = CatchUp{};
    g_wait_stats = WaitStats{};
    g_rejoin_tick_chunks = 0;
    g_guest_last_want = 0;
}

// Stops the network thread and closes the session's socket (in place: leaving, or connecting again).
void stop_network() {
    g_net_stop.store(true);
    wake_game_thread();
    if (g_connect_thread.joinable()) {
        g_connect_thread.join();
    }
    if (g_thread.joinable()) {
        g_thread.join();
    }
    if (g_socket >= 0) {
        conker::net::socket_close(g_socket);
        g_socket = -1;
    }
    conker::net::lobby::stop_discovery();
    if (is_host() && g_options.map) {
        conker::net::portmap::close();
    }
}

// The connect thread of a session started in place.
void connect_in_place() {
    const bool connected = g_options.host ? host_connect() : guest_connect();
    if (!connected) {
        if (g_socket >= 0) {
            conker::net::socket_close(g_socket);
            g_socket = -1;
        }
        conker::net::lobby::stop_discovery();
        if (!g_net_stop.load()) {
            std::lock_guard lock(g_place_mutex);
            if (g_place_message.empty()) {
                g_place_message = "Could not connect the session (the log says why).";
            }
        }
        g_place.store(kNoPlace);
        return;
    }
    if (is_host()) {
        host_in_place_start();
    }
    finish_connect();
    g_place.store(kConnected);
}

}  // namespace

bool connect() {
    if (!g_active) {
        return true;
    }
    for (int port = 0; port < kPorts; port++) {
        g_input_upto[port] = 0;
    }
#if defined(SIGHUP)
    const int stop_signals[] = {SIGTERM, SIGINT, SIGHUP};
#else
    const int stop_signals[] = {SIGTERM, SIGINT};
#endif
    for (int sig : stop_signals) {
        std::signal(sig, [](int caught) { g_stop_signal = caught; });
    }
    const bool connected = g_options.host ? host_connect() : guest_connect();
    for (int sig : stop_signals) {
        std::signal(sig, SIG_DFL);  // SDL puts its own in only over the default
    }
    if (!connected) {
        return false;
    }
    finish_connect();
    conker::display::set_logic_aspect(g_wide ? 16.0f / 9.0f : float(conker::display::kPictureWidth) / conker::display::kPictureHeight);
    g_thread.detach();
    return true;
}

// At a normal exit (the window closed, quit, Cmd-Q): a LEAVE to the other side, three times against
// loss, so that the host drops this guest's port at once instead of after kQuietGraceMs, and a guest
// learns the host is gone at once. Sent directly, past the simulated link. A crash or a kill sends
// nothing; the grace covers those.
void say_goodbye() {
    if (g_active.load() && g_socket >= 0 && g_stopped.load() != kHostLeft) {
        say_goodbye_now();
    }
    // A session started in place keeps its network (and connect) thread joinable; a std::thread still
    // joinable when exit() destroys it ends the program with std::terminate (an abort at every quit
    // from a session). Stopped and joined here, before the statics go.
    if (g_thread.joinable() || g_connect_thread.joinable()) {
        stop_network();
    }
}

void say_goodbye_now() {
    if (g_socket < 0 || g_stopped.load() == kHostLeft) {
        return;
    }
    std::unique_lock lock(g_mutex, std::defer_lock);
    for (int i = 0; i < 100 && !lock.try_lock(); i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!lock.owns_lock()) {
        return;
    }
    for (int copy = 0; copy < 3; copy++) {
        for (Peer& peer : g_peers) {
            if (is_host() && (!peer.started || (peer.dropped && !peer.rejoining))) {
                continue;
            }
            Writer w;
            begin(w, kLeave, is_host() ? peer.secret : g_secret);
            uint8_t out[64];
            const size_t size = seal::seal(peer.channel, w.data, w.size, out, sizeof(out));
            if (size > 0) {
                socket_send_to(g_socket, out, size, reinterpret_cast<const sockaddr*>(&peer.addr), peer.addr_len);
            }
        }
    }
}

int local_port() { return g_local_port; }

int local_controller(int port) {
    if (!g_active) {
        return port;
    }
    return port == g_local_port ? 0 : -1;
}

void use_session_save() {
    if (!g_active) {
        return;
    }
    const std::filesystem::path folder = data_dir() / "saves" / "net";
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    std::ofstream file(folder / save_name(), std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(g_eeprom.data()), static_cast<std::streamsize>(g_eeprom.size()));
    file.close();
    ultramodern::change_save_file(u8"net", kGameId);
    std::printf("[conker] net: playing on a copy of the host's save (saves/net/)\n");
}

Record boot_record() {
    Record record;
    for (int port = 0; port < kPorts; port++) {
        if ((g_start_ports & (1 << port)) != 0) {
            record.ports[port] = neutral_port();
        }
    }
    return record;
}

const std::string& host_settings() { return g_settings; }

namespace {


void note_wait(uint32_t frame, int64_t waited) {
    WaitStats& stats = g_wait_stats;
    if (stats.started == 0) {
        stats.started = now_ms();
    }
    stats.frames++;
    stats.waited_ms += waited;
    if (waited >= 50) {
        stats.stalls++;
        stats.stalled_ms += waited;
        stats.longest = std::max(stats.longest, waited);
    }
    if (frame % 900 != 0) {
        return;
    }
    int rtt = -1;
    {
        std::lock_guard lock(g_mutex);
        for (const Peer& peer : g_peers) {
            if (!peer.dropped) {
                rtt = std::max(rtt, peer.rtt);
            }
        }
    }
    const double seconds = (now_ms() - stats.started) / 1000.0;
    std::printf("[conker] net: frame %u: %.1f fps, waited %lld ms in all; %u stalls of 50 ms or more, %lld ms, the "
                "longest %lld ms; round trip %d ms\n",
                frame, seconds > 0 ? stats.frames / seconds : 0.0, static_cast<long long>(stats.waited_ms), stats.stalls,
                static_cast<long long>(stats.stalled_ms), static_cast<long long>(stats.longest), rtt);
    stats = WaitStats{};
    stats.started = now_ms();
}

// The host, on the game thread at the start of a frame merged for a snapshot: one for the guests
// that asked, compressed on a thread of its own. Not taken while a task is out (snapshot::save says
// why); then a later frame is marked for one.
void take_snapshot(uint8_t* rdram, uint32_t frame) {
    std::string why;
    const int64_t start = now_ms();
    std::vector<uint8_t> raw = conker::snapshot::save(rdram, conker::current_context(), frame, &why);
    if (raw.empty()) {
        std::printf("[conker] net: no snapshot at frame %u: %s; trying a later frame\n", frame, why.c_str());
        g_snap_requested.store(true);
        return;
    }
    std::thread([raw = std::move(raw), frame, start] {
        uLongf size = compressBound(static_cast<uLong>(raw.size()));
        std::vector<uint8_t> packed(4 + size);
        const uint32_t raw_size = static_cast<uint32_t>(raw.size());
        std::memcpy(packed.data(), &raw_size, 4);
        if (compress2(packed.data() + 4, &size, raw.data(), static_cast<uLong>(raw.size()), 1) != Z_OK) {
            std::fprintf(stderr, "[conker] net: the snapshot of frame %u could not be compressed\n", frame);
            return;
        }
        packed.resize(4 + size);
        std::printf("[conker] net: snapshot of frame %u: %zu KiB, %zu KiB compressed, in %lld ms\n", frame,
                    raw.size() / 1024, packed.size() / 1024, static_cast<long long>(now_ms() - start));
        std::lock_guard lock(g_mutex);
        g_snap_blob = std::make_shared<const std::vector<uint8_t>>(std::move(packed));
        g_snap_frame = frame;
        g_snap_serial++;
    }).detach();
}

// A rejoining guest, on the game thread at its first frame that needs the host's records: waits for
// the host's snapshot (asked for by the network thread), puts it in place and returns its frame, or
// returns frame unchanged to replay from frame 1 when it cannot.
uint32_t load_snapshot(uint8_t* rdram, uint32_t frame) {
    g_snap_tried = false;
    g_snap_waiting.store(true);
    const int64_t wait_start = now_ms();
    std::vector<uint8_t> packed;
    for (;;) {
        g_waiting.store(true);
        while (osRecvMesg(rdram, static_cast<int32_t>(kWaitQueue), NULLPTR, OS_MESG_NOBLOCK) == 0) {
        }
        {
            std::lock_guard lock(g_mutex);
            if (!g_snap_have.empty() && g_snap_have_count == g_snap_have.size()) {
                packed = g_snap_data;
                break;
            }
            if (g_stopped.load() != kPlaying || now_ms() - wait_start >= kSnapWaitMs) {
                break;
            }
        }
        osRecvMesg(rdram, static_cast<int32_t>(kWaitQueue), NULLPTR, OS_MESG_BLOCK);
    }
    g_waiting.store(false);
    g_snap_waiting.store(false);
    const int64_t received = now_ms();
    std::string why;
    std::vector<uint8_t> raw;
    if (packed.size() < 4) {
        why = "it did not come in " + std::to_string(kSnapWaitMs / 1000) + " s";
    } else {
        uint32_t raw_size;
        std::memcpy(&raw_size, packed.data(), 4);
        uLongf size = raw_size;
        // The size the host says, checked before anything is allocated for it.
        if (raw_size == 0 || raw_size > conker::snapshot::kMaxSize) {
            why = "its size (" + std::to_string(raw_size) + " bytes) is not a snapshot's";
        } else {
            raw.resize(raw_size);
            if (uncompress(raw.data(), &size, packed.data() + 4, static_cast<uLong>(packed.size() - 4)) != Z_OK ||
                size != raw_size) {
                why = "it did not unpack";
                raw.clear();
            }
        }
    }
    static const bool test_fail = std::getenv("CONKER_NET_TEST_SNAPSHOT_REFUSE") != nullptr;
    if (!raw.empty() && test_fail) {
        why = "CONKER_NET_TEST_SNAPSHOT_REFUSE";
        raw.clear();
    }
    uint32_t snap_frame = 0;
    if (!raw.empty()) {
        snap_frame = conker::snapshot::frame_of(raw);
        uint32_t host_upto, chunks_of;
        {
            std::lock_guard lock(g_mutex);
            host_upto = g_host_upto;
            chunks_of = g_snap_of;
        }
        // The frame is the host's word too, checked before the game's memory is touched: it becomes
        // this game's frame, and the frame hash history is grown to it.
        if (snap_frame <= static_cast<uint32_t>(g_delay) || snap_frame > kMaxFrame || snap_frame != chunks_of ||
            (host_upto != 0 && snap_frame > host_upto + kSnapFrameSlack)) {
            why = "its frame (" + std::to_string(snap_frame) + ") is not one the host is at";
            raw.clear();
        } else if (!conker::snapshot::load(rdram, conker::current_context(), raw, &why)) {
            raw.clear();
        }
    }
    std::lock_guard lock(g_mutex);
    g_snap_tried = true;
    if (raw.empty() && g_in_place) {
        // Nothing to replay from: the game here never was the session's.
        std::fprintf(stderr, "[conker] net: no snapshot (%s): cannot play in this session\n", why.c_str());
        set_place_message("The host's game did not reach this machine (" + why + ").");
        g_relaunch_started = true;  // nothing to rejoin with
        int playing = kPlaying;
        if (g_stopped.compare_exchange_strong(playing, kStoppedDropped)) {
            g_stopped_frame = frame;
        }
        return frame;
    }
    if (raw.empty()) {
        std::printf("[conker] net: no snapshot (%s): replaying from frame 1 instead\n", why.c_str());
        return frame;
    }
    static const bool test_skew = std::getenv("CONKER_NET_TEST_SNAPSHOT_SKEW") != nullptr;
    if (test_skew) {
        MEM_B(0, kseg0(0x80200000)) ^= 1;  // a game that differs from the host's after the snapshot
    }
    g_merged_upto = snap_frame - 1;
    g_host_upto = std::max(g_host_upto, snap_frame);
    // The history goes on from the snapshot's frame: the frames between were never played here (in
    // place, the frames before were this machine's own game: none of them is sent).
    g_own_hash_history.clear();
    g_own_hash_base = snap_frame;
    for (Hash& hash : g_own_hashes) {
        hash = Hash{};
    }
    g_hashes_sent_upto = 0;
    std::printf("[conker] net: snapshot of frame %u (%zu KiB) in place at frame %u, %.1f s after joining (%lld ms "
                "waiting for it); on from frame %u\n", snap_frame, packed.size() / 1024, frame,
                (received - g_join_start) / 1000.0, static_cast<long long>(received - wait_start), snap_frame);
    return snap_frame;
}

}  // namespace

void frame(uint8_t* rdram, Record* record) {
    uint32_t frame = record->frame;
    if (!g_queue_made.load()) {
        osCreateMesgQueue(rdram, static_cast<int32_t>(kWaitQueue), static_cast<int32_t>(kWaitSlots), kWaitSlotCount);
        g_queue_made.store(true);
    }
    if (!is_host() && g_snap_coming && !g_snap_tried && frame > static_cast<uint32_t>(g_delay)) {
        frame = load_snapshot(rdram, frame);
        record->frame = frame;
    }
    const Port local = record->ports[0];
    const Mouse local_mouse = record->mice[0];
    // CONKER_NET_TEST_DESYNC=<frame>: this peer changes a byte of game memory then, to see the stop.
    static const uint32_t test_desync = [] {
        const char* value = std::getenv("CONKER_NET_TEST_DESYNC");
        return value != nullptr ? static_cast<uint32_t>(std::strtoul(value, nullptr, 0)) : 0u;
    }();
    if (test_desync != 0 && frame == test_desync) {
        MEM_B(0, kseg0(0x80200000)) ^= 1;
        std::printf("[conker] net: CONKER_NET_TEST_DESYNC: a byte changed at frame %u\n", frame);
    }
    // CONKER_NET_TEST_HANG=<frame>[:<seconds>]: this peer's game stops for a while then (30 s by
    // default), its network going on, to see a stuck game dropped.
    static const std::pair<uint32_t, int> test_hang = [] {
        const char* value = std::getenv("CONKER_NET_TEST_HANG");
        if (value == nullptr) {
            return std::pair<uint32_t, int>{0, 0};
        }
        char* end;
        const uint32_t at = static_cast<uint32_t>(std::strtoul(value, &end, 0));
        return std::pair<uint32_t, int>{at, *end == ':' ? std::atoi(end + 1) : 30};
    }();
    // CONKER_NET_TEST_GUEST_PAUSE=1 (every peer): a guest's Start pauses a match as before, to test
    // the hand-over of a pause a dropped player opened.
    static const bool test_guest_pause = [] {
        const char* value = std::getenv("CONKER_NET_TEST_GUEST_PAUSE");
        return value != nullptr && value[0] == '1';
    }();
    // CONKER_NET_TEST_FORGED_LEAVE=<frame> (the host): a LEAVE from a guest's address and session but
    // without its secret must be ignored; the same with the secret drops the guest.
    static const uint32_t test_forged_leave = [] {
        const char* value = std::getenv("CONKER_NET_TEST_FORGED_LEAVE");
        return value != nullptr ? static_cast<uint32_t>(std::strtoul(value, nullptr, 0)) : 0u;
    }();
    if (is_host() && test_forged_leave != 0 && frame == test_forged_leave) {
        sockaddr_storage addr{};
        socklen_t addr_len = 0;
        uint64_t secret = 0;
        int port = -1;
        {
            std::lock_guard lock(g_mutex);
            for (const Peer& peer : g_peers) {
                if (peer.started && !peer.dropped) {
                    addr = peer.addr;
                    addr_len = peer.addr_len;
                    secret = peer.secret;
                    port = peer.port;
                    break;
                }
            }
        }
        if (port >= 0) {
            auto leave = [&](uint64_t with) {
                Writer w;
                begin(w, kLeave, with);
                handle_datagram(w.data, w.size, addr, addr_len);
                std::lock_guard lock(g_mutex);
                return g_drop_frame[port] != 0 && dropped_at(port, g_merged_upto + 1);
            };
            const bool forged = leave(secret ^ 0x5A5A5A5A5A5A5A5Aull);
            const bool zero = leave(0);
            const bool real = leave(secret);
            std::printf("[conker] net: forged LEAVE self-test: %s (a wrong secret %s, no secret %s, its own secret %s)\n",
                        !forged && !zero && real ? "PASS" : "FAIL", forged ? "dropped it" : "was ignored",
                        zero ? "dropped it" : "was ignored", real ? "dropped it" : "did not drop it");
        }
    }
    if (test_hang.first != 0 && frame == test_hang.first) {
        std::printf("[conker] net: CONKER_NET_TEST_HANG: the game stops for %d s at frame %u\n", test_hang.second, frame);
        std::this_thread::sleep_for(std::chrono::seconds(test_hang.second));
    }
    clear_depth(rdram);
    const uint64_t hash = hash_game(rdram);
    keep_memory(rdram, frame);
    Detail detail;
    detail.frame = frame;
    hash_blocks(rdram, detail.blocks);
    {
        std::lock_guard lock(g_mutex);
        g_details[frame % kDetailFrames] = detail;
        g_own_hashes[frame % kRing] = {frame, hash};
        g_own_hash_upto = frame;
        if (g_own_hash_history.empty() || frame < g_own_hash_base) {
            g_own_hash_history.clear();
            g_own_hash_base = frame;
        }
        if (g_own_hash_history.size() <= frame - g_own_hash_base) {
            g_own_hash_history.resize(frame - g_own_hash_base + 1, 0);
        }
        g_own_hash_history[frame - g_own_hash_base] = hash;
        if (is_host()) {
            compare_hashes(frame);
        }
        const uint32_t target = frame + static_cast<uint32_t>(g_delay);
        g_inputs[g_local_port][target % kRing] = {target, local, local_mouse};
        g_input_upto[g_local_port] = target;
        // The host sends when a frame is merged (new records); a guest, its new input. The tick resends.
        if (!is_host() || merge()) {
            send_updates();
        }
    }
    *record = boot_record();
    record->frame = frame;
    if (frame <= static_cast<uint32_t>(g_delay)) {
        return;
    }
    const int64_t wait_start = now_ms();
    bool snapshot_here = false;
    int64_t quiet_longest = 0;  // the host: the longest a port it waited for went unheard during the wait
    for (;;) {
        // Waiting is set, and old wake-ups taken, before looking: a record that comes after the look
        // wakes the wait below.
        g_waiting.store(true);
        while (osRecvMesg(rdram, static_cast<int32_t>(kWaitQueue), NULLPTR, OS_MESG_NOBLOCK) == 0) {
        }
        {
            std::lock_guard lock(g_mutex);
            if (g_stopped.load() != kPlaying) {
                break;  // stopped: every port as it was at boot, nothing pressed
            }
            const Merged& merged = g_merged[frame % kRing];
            if (g_merged_upto >= frame && merged.frame == frame) {
                snapshot_here = merged.snapshot != 0;
                bool was_unplugged[kPorts];
                std::copy(std::begin(g_unplugged), std::end(g_unplugged), was_unplugged);
                for (int port = 0; port < kPorts; port++) {
                    if ((merged.held & (1 << port)) == 0) {
                        record->ports[port] = Port{};  // nobody's: unplugged
                        record->mice[port] = Mouse{};
                        continue;
                    }
                    record->ports[port] = merged.ports[port];
                    record->mice[port] = merged.mice[port];
                    // A guest's Start does nothing in a match: only the host pauses, so that a guest
                    // who leaves cannot leave the game paused. Decided from the game's memory at the
                    // frame's start, the same on every peer.
                    if (port != 0 && !test_guest_pause && MEM_BU(0, kseg0(kScreenHandler)) == 0 &&
                        MEM_BU(0, kseg0(kMatchFlag)) != 0) {
                        record->ports[port].buttons &= static_cast<uint16_t>(~kStartButton);
                    }
                    // A dropped player's character stands still until the match is over; from then on
                    // (the match flag D_800BE616 clear at a frame's start, the same on every peer) the
                    // port reads as unplugged, so the join screen can offer a bot for it.
                    if ((merged.dropped & (1 << port)) == 0) {
                        // Playing (again). A port that reads as unplugged (one a newcomer took while the
                        // session ran, or one back after the match it was dropped from ended) is plugged
                        // in only outside a match, where the join screen takes it: a character cannot
                        // join a match under way. Decided the same way on every peer.
                        g_unplugged[port] = g_unplugged[port] && MEM_BU(0, kseg0(kMatchFlag)) != 0;
                    } else {
                        g_unplugged[port] = g_unplugged[port] || MEM_BU(0, kseg0(kMatchFlag)) == 0;
                        // A pause this player opened would wait for them for ever: port 1 works it.
                        if ((MEM_BU(0, kseg0(kPauseOpen)) != 0 || MEM_BU(0, kseg0(kPauseRunning)) != 0) &&
                            MEM_BU(0, kseg0(kPausePad)) == port) {
                            MEM_B(0, kseg0(kPausePad)) = 0;
                            std::printf("[conker] net: player %d left the game paused; player 1 has the pause now "
                                        "(frame %u)\n", port + 1, frame);
                        }
                    }
                    if (g_unplugged[port] != was_unplugged[port]) {
                        std::printf("[conker] net: player %d's port is %s at frame %u\n", port + 1,
                                    g_unplugged[port] ? "unplugged" : "plugged in", frame);
                    }
                    if (g_unplugged[port]) {
                        record->ports[port] = Port{};
                        record->mice[port] = Mouse{};
                    }
                }
                break;
            }
            if (now_ms() - wait_start >= kStallCountsMs) {
                const uint8_t missing = is_host() ? missing_for(frame) : (g_host_waiting_for != 0 ? g_host_waiting_for : 1);
                for (int port = 0; port < kPorts; port++) {
                    if ((missing & (1 << port)) != 0) {
                        g_stall_port.store(port);
                        break;
                    }
                }
                if (is_host()) {
                    for (const Peer& peer : g_peers) {
                        if ((missing & (1 << peer.port)) != 0) {
                            quiet_longest = std::max(quiet_longest, now_ms() - peer.last_heard);
                        }
                    }
                }
                g_stall_seen.store(now_ms());
            }
        }
        osRecvMesg(rdram, static_cast<int32_t>(kWaitQueue), NULLPTR, OS_MESG_BLOCK);
    }
    g_waiting.store(false);
    const int64_t waited = now_ms() - wait_start;
    note_wait(frame, waited);
    if (waited >= kStallCountsMs) {
        if (is_host()) {
            std::printf("[conker] net: waited %lld ms at frame %u for player %d (heard from %lld ms before at most)\n",
                        static_cast<long long>(waited), frame, g_stall_port.load() + 1, static_cast<long long>(quiet_longest));
        } else {
            std::printf("[conker] net: waited %lld ms at frame %u for the host\n", static_cast<long long>(waited), frame);
        }
    }
    // A frame merged for a snapshot: every peer's renderer starts it from RDRAM alone, as the guest
    // that loads the snapshot will, and the host takes it (the game's memory is still as the frame
    // started: nothing above has written to it but what every peer writes alike).
    if (snapshot_here) {
        conker::snapshot::request_framebuffer_discard();
        if (is_host()) {
            take_snapshot(rdram, frame);
        }
    }
    if (g_rejoin_from != 0 && !g_live_announced) {
        std::lock_guard lock(g_mutex);
        const uint32_t live = g_live_frame[g_local_port];
        if (live != 0 && frame >= live) {
            g_live_announced = true;
            std::printf("[conker] net: playing again at frame %u, %.1f s after joining\n", frame,
                        (now_ms() - g_join_start) / 1000.0);
        }
    }
    // A guest far behind the host (rejoining) runs the game's time fast, without sound, until it is
    // nearly level.
    if (!is_host()) {
        int64_t& catch_up_start = g_catch_up.start;
        uint32_t& catch_up_first = g_catch_up.first;
        int64_t& check_start = g_catch_up.check_start;
        int64_t& check_waited = g_catch_up.check_waited;
        bool& caught_up = g_catch_up.caught_up;
        int& stalled_catch_ups = g_catch_up.stalled;
        uint32_t host_upto;
        {
            std::lock_guard lock(g_mutex);
            host_upto = g_host_upto;
        }
        const uint32_t behind = host_upto > frame ? host_upto - frame : 0;
        // Catching up, the host's records are in before the game wants them. Waiting for them, the
        // host's records do not go as far as it says (a host can say anything), so this is not
        // catching up, only playing on without sound.
        bool not_catching_up = false;
        if (g_catching_up.load()) {
            check_waited += waited;
            if (now_ms() - check_start >= kCatchUpCheckMs) {
                not_catching_up = check_waited * 2 >= now_ms() - check_start;
                check_start = now_ms();
                check_waited = 0;
            }
        }
        if (!g_catching_up.load() && !caught_up && stalled_catch_ups < 3 && behind > kCatchUpFrom) {
            g_catching_up.store(true);
            catch_up_start = now_ms();
            catch_up_first = frame;
            check_start = catch_up_start;
            check_waited = 0;
            conker::audio::set_discard(true);
            static const uint32_t speed = [] {
                const char* value = std::getenv("CONKER_NET_CATCH_UP_SPEED");
                return value != nullptr && std::atoi(value) > 0 ? static_cast<uint32_t>(std::atoi(value)) : kCatchUpSpeed;
            }();
            ultramodern::set_speed_multiplier(speed);
            std::printf("[conker] net: %u frames behind the host at frame %u: catching up\n", behind, frame);
        } else if (g_catching_up.load() && (behind <= static_cast<uint32_t>(g_delay) + 4 || not_catching_up)) {
            caught_up = caught_up || !not_catching_up;
            stalled_catch_ups += not_catching_up ? 1 : 0;
            g_catching_up.store(false);
            ultramodern::set_speed_multiplier(1);
            conker::audio::set_discard(false);
            const double seconds = std::max<int64_t>(now_ms() - catch_up_start, 1) / 1000.0;
            if (not_catching_up) {
                std::printf("[conker] net: at frame %u the host says it is at %u, but this game has been waiting "
                            "for its records: not catching up\n", frame, host_upto);
            } else {
                std::printf("[conker] net: caught up at frame %u: %u frames in %.1f s, %.1f frames a second (%.1fx)\n",
                            frame, frame - catch_up_first, seconds, (frame - catch_up_first) / seconds,
                            (frame - catch_up_first) / seconds / 30.0);
            }
        }
    }
}

bool catching_up() { return g_catching_up.load(); }

std::string status_line() {
    if (!g_active) {
        return {};
    }
    char text[96];
    if (g_stopped.load() == kStoppedOutOfSync) {
        std::lock_guard lock(g_mutex);
        std::snprintf(text, sizeof(text), "OUT OF SYNC AT FRAME %u", g_stopped_frame);
        return text;
    }
    if (g_stopped.load() == kHostLeft) {
        return "HOST LEFT";
    }
    if (g_snap_waiting.load()) {
        std::lock_guard lock(g_mutex);
        const size_t total = g_snap_have.size();
        std::snprintf(text, sizeof(text), "REJOINING  SNAPSHOT %zu%%", total == 0 ? size_t{0} : g_snap_have_count * 100 / total);
        return text;
    }
    if (g_catching_up.load()) {
        std::lock_guard lock(g_mutex);
        std::snprintf(text, sizeof(text), "REJOINING  FRAME %u OF %u", g_merged_upto, g_host_upto);
        return text;
    }
    if (g_stopped.load() == kStoppedDropped) {
        std::lock_guard lock(g_mutex);
        std::snprintf(text, sizeof(text), "DROPPED AT FRAME %u", g_stopped_frame);
        return text;
    }
    const int64_t stalled = g_stall_seen.load();
    if (stalled != 0 && now_ms() - stalled < kStallShownMs) {
        std::snprintf(text, sizeof(text), "WAITING FOR P%d", g_stall_port.load() + 1);
        return text;
    }
    std::lock_guard lock(g_mutex);
    int ping = -1;
    std::string left;
    for (const Peer& peer : g_peers) {
        if (!peer.dropped) {
            ping = std::max(ping, peer.rtt);
        }
    }
    for (int port = 0; port < kPorts; port++) {
        if (g_drop_reason[port] != kNotDropped && dropped_at(port, g_merged_upto)) {
            left += "  P" + std::to_string(port + 1) +
                    (g_drop_reason[port] == kOutOfSync ? " OUT OF SYNC"
                     : g_drop_reason[port] == kNew      ? " JOINING"
                     : g_drop_reason[port] == kBack     ? " CATCHING UP"
                                                        : " LEFT");
        }
    }
    std::snprintf(text, sizeof(text), "P%d  PING %d  DELAY %d", g_local_port + 1, std::max(ping, 0), g_delay);
    return text + left;
}

std::string title_status() {
    if (!g_active) {
        return {};
    }
    return "online: " + status_line();
}

std::mutex g_session_menu_mutex;
SessionMenu g_session_menu;  // under g_session_menu_mutex

void set_session_menu(const SessionMenu& menu) {
    std::lock_guard lock(g_session_menu_mutex);
    g_session_menu = menu;
}

namespace {

// The lobby's session page: a dark panel in the middle of the picture, the title and rows twice the
// corner text's size, the chosen row white and the others grey, as the game's own menus colour them.
void draw_session_menu(ListWriter& list, const SessionMenu& menu, int kWidth, int kHeight) {
    constexpr int kBig = 2;
    const auto centered = [&](const std::string& text, int y, int scale, uint32_t color) {
        const int width = static_cast<int>(text.size()) * 6 * scale - scale;
        const int x = std::max(0, (kWidth - width) / 2);
        list.command(0xE7000000u, 0);
        list.command(0xFA000000u, 0x000000FFu);
        draw_text(list, text, x + scale, y + scale, scale);  // the shadow
        list.command(0xE7000000u, 0);
        list.command(0xFA000000u, color);
        draw_text(list, text, x, y, scale);
    };
    const int height = 8 * kBig + 6 + static_cast<int>(menu.lines.size()) * 10 + 6 +
                       static_cast<int>(menu.rows.size()) * 9 * kBig + (menu.hint.empty() ? 0 : 12);
    int y = std::max(4, (kHeight - height) / 2) - 8;
    // As the game's pause: the picture dimmed behind (the colour blended over what is there by the
    // primitive alpha), the title gold, the chosen row white and the others grey, with no box.
    list.command(0xE7000000u, 0);
    list.command(0xE200001Cu, 0x00504000u);   // G_SETOTHERMODE_L: FORCE_BL, in * alpha + memory * (1 - alpha)
    list.command(0xFA000000u, 0x000000A0u);   // black, about 60%
    list.rect(0, 0, kWidth, kHeight);
    list.command(0xE7000000u, 0);
    list.command(0xE200001Cu, 0);             // opaque again for the text
    y += 8;
    centered(menu.title, y, kBig, 0xCEC461FFu);
    y += 8 * kBig + 6;
    for (const std::string& line : menu.lines) {
        centered(line, y, 1, 0x9A9A9AFFu);
        y += 10;
    }
    y += 6;
    for (size_t i = 0; i < menu.rows.size(); i++) {
        centered(menu.rows[i], y, kBig, static_cast<int>(i) == menu.chosen ? 0xFFFFFFFFu : 0x9A9A9AFFu);
        y += 9 * kBig;
    }
    if (!menu.hint.empty()) {
        centered(menu.hint, y + 4, 1, 0x9A9A9AFFu);
    }
}

}  // namespace

uint32_t overlay_display_list(uint8_t* rdram, uint32_t game_list) {
    SessionMenu menu;
    {
        std::lock_guard lock(g_session_menu_mutex);
        menu = g_session_menu;
    }
    if (!g_active && menu.rows.empty()) {
        return game_list;
    }
    // The corner shows only what is wrong (a player left or catching up, a stall, out of sync), not
    // the usual "P1  PING 0  DELAY 2", which the window's title still gives.
    std::string text = g_active ? status_line() : std::string();
    if (const size_t delay = text.find("  DELAY "); text.rfind("P", 0) == 0 && delay != std::string::npos) {
        const size_t rest = text.find("  P", delay);  // the players dropped, after the delay
        text = rest == std::string::npos ? std::string() : text.substr(rest + 2);
    }
    if (text.empty() && menu.rows.empty()) {
        return game_list;
    }
    // Only into a 16-bit frame buffer at the top of RDRAM, where the game keeps them: 320 wide in
    // play, 292 (by 216 lines) on the title and its menus.
    const ColorImage image = last_color_image(rdram, game_list);
    const int width = static_cast<int>(image.width);
    const int height = width == 292 ? 216 : 240;
    if (image.size != 2 || (width != 320 && width != 292) || image.address < kHashEnd ||
        image.address + static_cast<uint32_t>(width * height * 2) > 0x400000) {
        return game_list;
    }
    static int which = 0;
    which ^= 1;
    const uint32_t start = kOverlayLists[which];
    ListWriter list{rdram, start, start + kOverlaySize};
    list.command(0xDE000000u, game_list & 0x3FFFFFF);  // G_DL, called: the game's frame
    list.command(0xE7000000u, 0);                       // G_RDPPIPESYNC
    list.command(0xE3000A01u, 0);                       // G_SETOTHERMODE_H: one-cycle
    list.command(0xE200001Cu, 0);                       // G_SETOTHERMODE_L: no blending, no depth
    list.command(0xFCFFFFFFu, 0xFFFDF6FBu);             // G_SETCOMBINE: the primitive colour
    list.command(0xED000000u, (static_cast<uint32_t>(width) << 14) | (static_cast<uint32_t>(height) << 2));  // G_SETSCISSOR: the whole picture
    const bool alert = true;                            // the corner only says what is wrong
    list.command(0xFA000000u, 0x000000FFu);             // G_SETPRIMCOLOR: the shadow
    const int text_y = height - (240 - kTextY);
    draw_text(list, text, kTextX + 1, text_y + 1);
    list.command(0xE7000000u, 0);
    list.command(0xFA000000u, alert ? 0xFF6040FFu : 0xFFFFFFFFu);
    draw_text(list, text, kTextX, text_y);
    if (!menu.rows.empty()) {
        draw_session_menu(list, menu, width, height);
    }
    list.command(0xE9000000u, 0);                       // G_RDPFULLSYNC
    MEM_W(0, kseg0(list.at)) = static_cast<int32_t>(0xDF000000u);  // G_ENDDL
    MEM_W(4, kseg0(list.at)) = 0;
    return start & 0x3FFFFFF;
}


// --- sessions started in place ----------------------------------------------------------------------

bool start_in_place(const std::vector<std::string>& net_args) {
    if (g_active.load() || g_place.load() != kNoPlace) {
        return false;
    }
    if (g_connect_thread.joinable()) {
        g_connect_thread.join();  // a connect that failed before
    }
    const bool internet = g_options.internet;
    g_options = Options{};
    g_options.internet = internet;
    g_expected = seal::Expected{};
    std::vector<std::string> args = net_args;
    std::vector<char*> argv;
    for (std::string& arg : args) {
        argv.push_back(arg.data());
    }
    std::vector<char*> kept;
    if (!parse_net_options(static_cast<int>(argv.size()), argv.data(), &kept) || (!g_options.host && !g_options.join)) {
        set_place_message("The session's options were not right (the log says why).");
        return false;
    }
    reset_session();
    set_place_message("");
    g_in_place = true;
    g_net_stop.store(false);
    g_rejoin_port = 0;
    g_rejoin_secret_kept = 0;
    g_rejoin_count = 0;
    g_place.store(kConnecting);
    std::printf("[conker] net: connecting the session in place (%s)\n", g_options.host ? "hosting" : "joining");
    register_goodbye();
    g_connect_thread = std::thread(connect_in_place);
    return true;
}

InPlace in_place_state(std::string* message) {
    if (message != nullptr) {
        std::lock_guard lock(g_place_mutex);
        *message = g_place_message;
    }
    switch (g_place.load()) {
        case kConnecting:
        case kConnected:
        case kEntering: return InPlace::kStarting;
        case kInSession:
        case kLeaving: return InPlace::kPlaying;
        default: return InPlace::kNone;
    }
}

bool in_place() { return g_in_place && g_place.load() != kNoPlace; }

void cancel_in_place() {
    if (g_place.load() == kConnecting) {
        g_net_stop.store(true);
        if (g_connect_thread.joinable()) {
            g_connect_thread.join();
        }
        g_place.store(kNoPlace);
        g_in_place = false;
        return;
    }
    request_leave();
}

void request_leave() {
    if (g_in_place && g_place.load() == kInSession) {
        g_leave_requested.store(true);
        wake_game_thread();  // a game waiting for input notices
    }
}

namespace {

// The game thread, kEntering, once this machine's own game is kept: into the session.
void enter_session() {
    const bool first = !g_own_kept;
    if (first) {
        g_own_settings = conker::frame_input::settings_header();
        g_own_kept = true;
        conker::display::keep_saved_picture();  // the session's widescreen and sharp are not saved
    }
    // The session's copy of the host's save (a guest's then becomes the host's with the snapshot).
    const std::filesystem::path folder = data_dir() / "saves" / "net";
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    {
        std::ofstream file(folder / save_name(), std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(g_eeprom.data()), static_cast<std::streamsize>(g_eeprom.size()));
    }
    ultramodern::change_save_file(u8"net", kGameId);
    if (!is_host()) {
        conker::frame_input::apply_settings(g_settings, "net: the host's setting");
    }
    conker::display::set_logic_aspect(g_wide ? 16.0f / 9.0f : float(conker::display::kPictureWidth) / conker::display::kPictureHeight);
    conker::frame_input::set_frame(0);  // the session's frames count from 1, as on every peer
    g_stop_seen = 0;
    g_leave_requested.store(false);
    g_rejoin_requested.store(false);
    g_active.store(true);
    g_place.store(kInSession);
    std::printf("[conker] net: in the session as player %d (started in place, on a copy of the host's save)\n",
                g_local_port + 1);
}

// The game thread: connecting again after the host dropped this machine (its own game is still kept).
void rejoin_in_place() {
    g_rejoin_requested.store(false);
    const int port = g_local_port + 1;
    const uint64_t secret = g_secret;
    const int count = g_rejoin_count + 1;
    g_active.store(false);
    stop_network();
    conker::audio::set_discard(false);
    ultramodern::set_speed_multiplier(1);
    reset_session();
    g_rejoin_port = port;
    g_rejoin_secret_kept = secret;
    g_rejoin_count = count;
    g_net_stop.store(false);
    g_place.store(kConnecting);
    std::printf("[conker] net: connecting again to rejoin the session as player %d\n", port);
    register_goodbye();
    g_connect_thread = std::thread(connect_in_place);
}

// The game thread: out of the session, and this machine's own game back as it was before it. False
// while the game cannot take its snapshot back yet (the next frame tries again).
bool leave_session(uint8_t* rdram, recomp_context* ctx) {
    if (g_place.load() != kLeaving) {
        g_active.store(false);
        say_goodbye_now();
        stop_network();
        conker::audio::set_discard(false);
        ultramodern::set_speed_multiplier(1);
        g_place.store(kLeaving);
    }
    ultramodern::change_save_file(u8"", kGameId);
    std::string why;
    if (!g_return.empty() && !conker::snapshot::load(rdram, ctx, g_return, &why)) {
        std::printf("[conker] net: this machine's own game cannot come back yet (%s); trying the next frame\n", why.c_str());
        ultramodern::change_save_file(u8"net", kGameId);
        return false;
    }
    conker::frame_input::leave_fixed_step();
    conker::frame_input::restore_settings(g_own_settings);
    conker::display::release_saved_picture();
    conker::display::set_logic_aspect(0.0f);
    reset_session();
    g_return.clear();
    g_return.shrink_to_fit();
    g_own_settings.clear();
    g_own_kept = false;
    g_in_place = false;
    g_leave_requested.store(false);
    g_rejoin_requested.store(false);
    g_place.store(kNoPlace);
    std::printf("[conker] net: left the session; this machine's own game is back\n");
    return true;
}

}  // namespace

void frame_start(uint8_t* rdram) {
    if (g_place.load() == kConnected && conker::frame_input::enter_fixed_step(rdram)) {
        g_place.store(kEntering);
    }
}

void before_input(uint8_t* rdram, recomp_context* ctx) {
    const int place = g_place.load();
    if (place == kEntering) {
        if (!g_own_kept) {
            std::string why;
            g_return = conker::snapshot::save(rdram, ctx, 0, &why);
            if (g_return.empty()) {
                std::printf("[conker] net: cannot keep this machine's own game yet (%s); trying the next frame\n", why.c_str());
                return;
            }
        }
        enter_session();
        return;
    }
    if (place == kLeaving) {
        leave_session(rdram, ctx);
        return;
    }
    // Connecting again to rejoin, the game goes on as it is (its port gives nothing); connecting failed:
    // this machine's own game back.
    if (place == kNoPlace && g_own_kept) {
        g_place.store(kLeaving);
        leave_session(rdram, ctx);
        return;
    }
    if (place != kInSession || !g_in_place) {
        return;
    }
    if (g_rejoin_requested.load()) {
        rejoin_in_place();
        return;
    }
    // The session over here (the host left, this machine dropped for good, the host's game never came):
    // the reason stays in the corner for a moment, then this machine's own game comes back.
    if (g_stopped.load() != kPlaying && g_stop_seen == 0) {
        g_stop_seen = now_ms();
        std::lock_guard lock(g_place_mutex);
        if (g_place_message.empty()) {
            g_place_message = status_line();
        }
    }
    // (A drop this machine can rejoin from asks for it within a few milliseconds: relaunch_to_rejoin.)
    constexpr int64_t kStoppedShownMs = 4000;
    if (g_leave_requested.load() || (g_stop_seen != 0 && now_ms() - g_stop_seen >= kStoppedShownMs)) {
        leave_session(rdram, ctx);
    }
}

}  // namespace conker::net
