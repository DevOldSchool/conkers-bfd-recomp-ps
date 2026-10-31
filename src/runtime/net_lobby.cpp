// The online lobby's network side; see net_lobby.hpp.
#include "runtime/net_lobby.hpp"

#include <algorithm>
#include <cmath>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <thread>

#include "platform/input.hpp"
#include "runtime/frame_input.hpp"
#include "runtime/game.hpp"
#include "runtime/net_codes.hpp"
#include "runtime/net_play.hpp"
#include "runtime/net_socket.hpp"

namespace conker::net::lobby {

namespace {

// PROBE and OFFER keep the first version's magic and layout (an OFFER only gains fields at its end), so
// that a game of another version still shows in the LAN list as one ("a different version"); the rest
// changed with the security fixes  and has its own magic.
constexpr uint32_t kDiscoveryMagic = 0x314C4B43;  // "CKL1"
constexpr uint32_t kMagic = 0x324C4B43;           // "CKL2"
enum Type : uint8_t { kProbe = 1, kOffer, kJoin, kReject, kState, kHeartbeat, kStart, kGo, kLeave, kCookie };
// The lobby's version, in every OFFER: games of another one cannot play together.
// 3: each member carries the host's per-member secret in HEARTBEAT/GO/LEAVE.
// 4: the host carries it too, in every STATE, START, LEAVE and REJECT to a member.
// 5: JOIN carries the version too, and the host refuses another: two builds with the same build text
// ("<commit>-dirty") but other lobby code got past each other's checks.
// 6: the session's LEAVE and its "coming back" drop reason; a late guest is dropped after a short grace.
// 7: a pad goes out as its make and model ("DualSense"), never its name (often its owner's), and what
// comes in is held to that list.
// 8: the session's records go as changes from the frame before (session magic CKN7).
// 9: fewer session packets, hashes and drop blocks (session magic CKN8).
// 10: a packet's first record as changes from the frame acknowledged (session magic CKN9).
constexpr uint16_t kProtocol = 10;
// A PROBE is padded to this size and an OFFER is never bigger than the PROBE it answers, so that a
// PROBE with a forged sender gets no more sent to that address than it took to send.
constexpr size_t kProbeSize = 400;
constexpr uint32_t kCookiePurpose = 0x4C;  // address_cookie's purpose for a lobby JOIN
constexpr int kMaxPlayers = 4;
constexpr int64_t kTickMs = 250;           // STATE and HEARTBEAT
constexpr int64_t kGoneAfterMs = 6000;     // a lobby member not heard from for this long has left
constexpr int64_t kProbeEveryMs = 1000;    // browsing: a broadcast PROBE this often
constexpr int64_t kGameKeptMs = 4000;      // a LAN game not heard from for this long leaves the list
constexpr int64_t kProbeGiveUpMs = 4000;   // JOIN: no OFFER from the address in this long
constexpr int64_t kStartWaitMs = 3000;     // the host starts again after every GO, or this long
constexpr size_t kMaxGames = 32;           // the LAN list's games, at most

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct Writer {
    uint8_t data[1400];
    size_t size = 0;
    void u8(uint8_t v) { if (size < sizeof(data)) data[size++] = v; }
    void u16(uint16_t v) { u8(v & 0xFF); u8(v >> 8); }
    void u32(uint32_t v) { u16(v & 0xFFFF); u16(v >> 16); }
    void u64(uint64_t v) { u32(static_cast<uint32_t>(v)); u32(static_cast<uint32_t>(v >> 32)); }
    void text(const std::string& s, size_t max) {
        const size_t n = std::min(s.size(), max);
        u16(static_cast<uint16_t>(n));
        for (size_t i = 0; i < n; i++) u8(static_cast<uint8_t>(s[i]));
    }
    explicit Writer(Type type) { u32(type == kProbe || type == kOffer ? kDiscoveryMagic : kMagic); u8(type); }
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
    std::string text(size_t max) {
        const size_t n = u16();
        if (n > max || at + n > size) { ok = false; return {}; }
        std::string s(reinterpret_cast<const char*>(data + at), n);
        at += n;
        return s;
    }
};

// Names travel and show as printable ASCII, at most 16 characters.
std::string clean_name(const std::string& text) {
    std::string out;
    for (char c : text) {
        if (c >= 32 && c < 127 && out.size() < 16) {
            out += c;
        }
    }
    out.erase(0, out.find_first_not_of(' '));
    out.erase(out.find_last_not_of(' ') + 1);
    return out.empty() ? "Player" : out;
}

void send_to(int socket, const sockaddr_storage& addr, socklen_t len, const Writer& w) {
    sockaddr_storage own{};
    socklen_t own_len = sizeof(own);
    getsockname(socket, reinterpret_cast<sockaddr*>(&own), &own_len);
    if (own.ss_family == AF_INET6 && addr.ss_family == AF_INET) {
        // A dual-stack socket sends to an IPv4 address as an IPv4-mapped IPv6 one.
        const auto& v4 = reinterpret_cast<const sockaddr_in&>(addr);
        sockaddr_in6 mapped{};
        mapped.sin6_family = AF_INET6;
        mapped.sin6_port = v4.sin_port;
        mapped.sin6_addr.s6_addr[10] = 0xFF;
        mapped.sin6_addr.s6_addr[11] = 0xFF;
        std::memcpy(mapped.sin6_addr.s6_addr + 12, &v4.sin_addr, 4);
#if defined(__APPLE__)
        mapped.sin6_len = sizeof(mapped);
#endif
        socket_send_to(socket, w.data, w.size, reinterpret_cast<const sockaddr*>(&mapped), sizeof(mapped));
        return;
    }
    socket_send_to(socket, w.data, w.size, reinterpret_cast<const sockaddr*>(&addr), len);
}

bool same_address(const sockaddr_storage& a, socklen_t, const sockaddr_storage& b, socklen_t) {
    return address_with_port(a, 0) == address_with_port(b, 0);
}


// A dual-stack UDP socket on `port` (0: any); -1 if it cannot be had.
int open_socket(uint16_t port) {
    int s = static_cast<int>(socket(AF_INET6, SOCK_DGRAM, 0));
    if (s >= 0) {
        socket_set_option(s, IPPROTO_IPV6, IPV6_V6ONLY, 0);
        sockaddr_in6 addr{};
        addr.sin6_family = AF_INET6;
        addr.sin6_addr = in6addr_any;
        addr.sin6_port = htons(port);
        if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            socket_close(s);
            s = -1;
        }
    }
    if (s < 0) {
        s = static_cast<int>(socket(AF_INET, SOCK_DGRAM, 0));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(port);
        if (s >= 0 && bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            socket_close(s);
            s = -1;
        }
    }
    if (s >= 0) {
        socket_prepare(s);  // not handed on when the game starts again into the session
    }
    return s;
}

// The IPv4 socket broadcast PROBEs go out on, and the discovery port's (shared by every game on
// this machine that hosts: each gets a copy of a broadcast).
int open_ipv4_socket(uint16_t port, bool shared) {
    int s = static_cast<int>(socket(AF_INET, SOCK_DGRAM, 0));
    if (s < 0) {
        return -1;
    }
    socket_set_option(s, SOL_SOCKET, SO_BROADCAST, 1);
    if (shared) {
        // Windows has no SO_REUSEPORT: there SO_REUSEADDR alone shares the port.
        socket_set_option(s, SOL_SOCKET, SO_REUSEADDR, 1);
#if defined(SO_REUSEPORT)
        socket_set_option(s, SOL_SOCKET, SO_REUSEPORT, 1);
#endif
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        socket_close(s);
        return -1;
    }
    socket_prepare(s);
    return s;
}

// What other players see of this machine's pad: its make and model only. macOS names a Bluetooth pad
// after its owner ("<Name>'s DualSense"), so the name stays on this machine (own_pad).
std::string wire_pad() {
    const std::string model = conker::input::controller_model();
    return model.empty() ? "Keyboard" : model;
}

// This machine's own row: the pad's full name, shown here only.
std::string own_pad() {
    const std::string name = conker::input::controller_name();
    return name.empty() ? "Keyboard" : printable(name, 40);
}

// A pad as another machine sent it, held to the makes and models wire_pad sends: an older or changed
// game that sent a name gets "Controller" shown and passed on instead.
std::string known_pad(const std::string& text) {
    static const char* const kModels[] = {"DualSense", "DualShock 4", "DualShock 3", "PlayStation", "Xbox",
                                          "Switch Pro", "Nintendo", "Keyboard", "Controller"};
    for (const char* model : kModels) {
        if (text == model) {
            return text;
        }
    }
    return "Controller";
}

// The host's settings ("# switch LABEL=0|1" lines) as "LABEL on/off" lines.
std::string settings_summary(const std::string& header) {
    std::string out;
    size_t at = 0;
    while (at < header.size()) {
        size_t end = header.find('\n', at);
        if (end == std::string::npos) {
            end = header.size();
        }
        const std::string line = header.substr(at, end - at);
        at = end + 1;
        char label[64];
        int on = 0;
        if (std::sscanf(line.c_str(), "# switch %63[^=]=%d", label, &on) == 2) {
            out += std::string(label) + (on != 0 ? "  on\n" : "  off\n");
        }
    }
    return out;
}

// The lines of `theirs` (settings_summary) that differ from this machine's own.
std::string settings_differ(const std::string& theirs) {
    const std::string own = settings_summary(conker::frame_input::settings_header());
    std::string out;
    size_t at = 0;
    while (at < theirs.size()) {
        size_t end = theirs.find('\n', at);
        if (end == std::string::npos) {
            end = theirs.size();
        }
        const std::string line = theirs.substr(at, end - at + 1);
        at = end + 1;
        if (!line.empty() && own.find(line) == std::string::npos) {
            out += line;
        }
    }
    return out;
}

// Why this machine could not play with a game of this build, OS, ROM and lobby version ("" if it can).
std::string mismatch(const std::string& build_in, const std::string& os, uint64_t rom, uint16_t protocol) {
    const std::string build = printable(build_in, 40);
    if (build_in != conker::net::build_text() || protocol != kProtocol) {
        return "a different version of the game (the host runs " + build + ", this machine " +
               conker::net::build_text() + "); everyone needs the same build";
    }
    if (os != conker::net::os_text()) {
        return "a different system (the host runs " + printable(os, 40) + ", this machine " + conker::net::os_text() +
               "); for now everyone needs the same OS and CPU";
    }
    if (rom != conker::net::rom_hash()) {
        return "a different ROM; everyone needs Conker's Bad Fur Day (US)";
    }
    return "";
}

// --- state (under g_mutex) ---------------------------------------------------------------------------

struct Member {
    sockaddr_storage addr{};
    socklen_t len = 0;
    uint32_t nonce = 0;
    uint64_t secret = 0;   // the host's, told to this member in STATE and required in its HEARTBEAT/GO/LEAVE
    std::string name;
    std::string pad;
    int ping = -1;
    std::vector<int> pings;  // every ping it told, for the session's input delay (session_delay)
    bool ready = false;
    bool go = false;
    int64_t last_heard = 0;
    uint32_t echo = 0;     // its last HEARTBEAT's time, echoed in the next STATE for its ping
    int64_t echo_at = 0;   // when that came
};

struct Found {
    Game game;
    uint32_t id = 0;   // the host's, the same whichever address its answer came from
    sockaddr_storage addr{};
    socklen_t len = 0;
    int64_t heard = 0;
};

std::mutex g_mutex;
Mode g_mode = Mode::Idle;
int g_socket = -1;             // hosting: the game port; joining: any port
int g_probe_socket = -1;       // browsing: broadcasts out, OFFERs in
int g_discovery_socket = -1;   // hosting: the discovery port
uint16_t g_port = 0;
uint32_t g_nonce = 0;          // browsing: the LAN list's PROBEs (broadcast, so anyone on the network has it)
uint64_t g_session_token = 0;  // hosting: the session's token, given to each guest in START
std::string g_error;
std::string g_notice;
bool g_browsing = false;
int64_t g_last_probe = 0;
std::vector<Found> g_found;
std::thread g_thread;
bool g_thread_running = false;

// Hosting.
std::vector<Member> g_members;
std::vector<std::string> g_refused;  // the addresses (without port) of machines the host removed
int64_t g_start_at = 0;        // when START was pressed (0: not yet)
bool g_map_tried = false;

// Joining.
std::string g_target_text;
uint32_t g_join_nonce = 0;     // joining: this JOIN's own, never broadcast (the LAN list's g_nonce is)
sockaddr_storage g_host_addr{};
socklen_t g_host_len = 0;
int64_t g_probe_since = 0;
bool g_target_in_lobby = false;  // the game being joined answered its PROBE from its lobby
int64_t g_last_probe_to_host = 0;
bool g_joined_lobby = false;
bool g_ready = false;
int64_t g_host_heard = 0;
int g_ping = -1;
uint64_t g_join_cookie = 0;      // joining: the host's COOKIE for this machine's address
uint64_t g_member_secret = 0;    // joined: the host's per-member secret, from the first STATE; in every control
                                 // packet either way after it
uint64_t g_start_token = 0;      // joined: the session token START gave
std::vector<Player> g_players;  // joined: the host's list
std::string g_settings;         // joined: the host's settings summary
int g_start_port = -1;          // joined: the port START gave this machine
int g_start_players = 0;
int64_t g_go_at = 0;

std::filesystem::path settings_file() {
    return std::filesystem::current_path() / "conker-recomp-data" / "online.txt";
}

std::string read_setting(const std::string& key) {
    std::ifstream file(settings_file());
    std::string line;
    while (std::getline(file, line)) {
        if (line.rfind(key + "=", 0) == 0) {
            return line.substr(key.size() + 1);
        }
    }
    return "";
}

void write_setting(const std::string& key, const std::string& value) {
    std::vector<std::string> lines;
    {
        std::ifstream file(settings_file());
        std::string line;
        while (std::getline(file, line)) {
            if (line.rfind(key + "=", 0) != 0) {
                lines.push_back(line);
            }
        }
    }
    lines.push_back(key + "=" + value);
    std::error_code error;
    std::filesystem::create_directories(settings_file().parent_path(), error);
    std::ofstream file(settings_file());
    for (const std::string& line : lines) {
        file << line << "\n";
    }
}

void close_sockets() {
    for (int* s : {&g_socket, &g_probe_socket, &g_discovery_socket}) {
        if (*s >= 0) {
            socket_close(*s);
            *s = -1;
        }
    }
}

Offer lobby_offer() {
    Offer offer;
    offer.name = clean_name(read_setting("name"));
    offer.port = g_port;
    offer.playing = false;
    offer.players = 1 + static_cast<int>(g_members.size());
    offer.joinable = offer.players < kMaxPlayers && g_start_at == 0;
    offer.id = g_nonce;
    return offer;
}

void write_offer(Writer& w, uint32_t nonce, const Offer& offer) {
    w.u32(nonce);
    w.u32(offer.id);
    w.text(offer.name, 16);
    w.text(conker::net::build_text(), 200);
    w.text(conker::net::os_text(), 64);
    w.u64(conker::net::rom_hash());
    w.u16(offer.port);
    w.u8(offer.playing ? 1 : 0);
    w.u8(static_cast<uint8_t>(offer.players));
    w.u8(offer.joinable ? 1 : 0);
    w.u16(kProtocol);
}

// Answers a PROBE (its reader past the type) with an OFFER, if the PROBE was padded to at least the
// OFFER's size; one from an older game, unpadded, gets nothing.
void offer_to(int socket, Reader& r, size_t probe_size, const sockaddr_storage& from, socklen_t from_len, const Offer& offer) {
    const uint32_t nonce = r.u32();
    if (!r.ok) {
        return;
    }
    Writer w(kOffer);
    write_offer(w, nonce, offer);
    if (w.size <= probe_size) {
        send_to(socket, from, from_len, w);
    }
}

void send_probe(int socket, const sockaddr_storage& to, socklen_t len, uint32_t nonce) {
    Writer w(kProbe);
    w.u32(nonce);
    while (w.size < kProbeSize) {
        w.u8(0);
    }
    send_to(socket, to, len, w);
}

// Hosting: what this lobby's players look like, to the host and in STATE.
std::vector<Player> host_players() {
    std::vector<Player> players;
    Player me;
    me.name = clean_name(read_setting("name"));
    me.pad = own_pad();  // on this screen only: send_state sends wire_pad()
    me.ping = 0;
    me.ready = true;
    me.host = true;
    me.you = true;
    players.push_back(me);
    for (const Member& m : g_members) {
        Player p;
        p.name = m.name;
        p.pad = m.pad;
        p.ping = m.ping;
        p.ready = m.ready;
        players.push_back(p);
    }
    return players;
}

// STATE goes to each member on the lobby's tick only, never in answer to a datagram, so that nothing
// sent makes the host send more.
void send_state(const Member& to, int64_t now) {
    Writer w(kState);
    w.u32(to.nonce);
    w.u64(to.secret);  // this member's secret: required in its HEARTBEAT/GO/LEAVE
    w.u32(to.echo);
    w.u16(static_cast<uint16_t>(to.echo != 0 ? std::clamp<int64_t>(now - to.echo_at, 0, 0xFFFF) : 0));
    const std::vector<Player> players = host_players();
    w.u8(static_cast<uint8_t>(players.size()));
    for (size_t i = 0; i < players.size(); i++) {
        const Player& p = players[i];
        const bool to_them = i > 0 && &g_members[i - 1] == &to;
        w.text(p.name, 16);
        w.text(i == 0 ? wire_pad() : p.pad, 40);  // members' pads are known_pad() already
        w.u16(static_cast<uint16_t>(p.ping < 0 ? 0xFFFF : std::min(p.ping, 9999)));
        w.u8((p.ready ? 1 : 0) | (p.host ? 2 : 0) | (to_them ? 4 : 0));
    }
    w.text(settings_summary(conker::frame_input::settings_header()), 1000);
    send_to(g_socket, to.addr, to.len, w);
}

// An OFFER: for the LAN list (an answer to the broadcast PROBEs' nonce) or, with the JOIN's own nonce,
// from the game being joined, which alone moves the JOIN on: the broadcast nonce is no one's secret.
void on_found(const sockaddr_storage& from, socklen_t from_len, Reader& r, bool to_join) {
    Found found;
    if (r.u32() != (to_join ? g_join_nonce : g_nonce)) {
        return;  // not an answer to this machine's PROBE
    }
    found.id = r.u32();
    found.game.name = clean_name(r.text(16));
    const std::string build = r.text(200);
    const std::string os = r.text(64);
    const uint64_t rom = r.u64();
    const uint16_t port = r.u16();
    found.game.playing = r.u8() != 0;
    found.game.players = r.u8();
    found.game.joinable = r.u8() != 0;
    const uint16_t protocol = r.size - r.at >= 2 ? r.u16() : 1;  // an older game's OFFER ends before it
    if (!r.ok) {
        return;
    }
    found.addr = from;
    found.len = from_len;
    if (from.ss_family == AF_INET) {
        reinterpret_cast<sockaddr_in&>(found.addr).sin_port = htons(port);
    } else {
        reinterpret_cast<sockaddr_in6&>(found.addr).sin6_port = htons(port);
    }
    found.game.address = address_with_port(found.addr, found.len);
    found.game.problem = mismatch(build, os, rom, protocol);
    if (found.game.problem.empty() && !found.game.joinable) {
        found.game.problem = found.game.playing ? "it is in a match that takes no one new (only its own lobby's "
                                                  "players, or it is full). Ask them to go back to the lobby, or wait"
                                                : "the lobby is full or starting";
    }
    found.heard = now_ms();
    // The game being joined: go on into its lobby, or straight into its session.
    if (to_join && g_mode == Mode::Probing && found.game.address == address_with_port(g_host_addr, g_host_len)) {
        if (!found.game.problem.empty()) {
            g_error = "Cannot join " + found.game.name + ": " + found.game.problem + ".";
            g_mode = Mode::Idle;
        } else if (found.game.playing) {
            g_notice = "Joining " + found.game.name + "'s match: starting again into the session...";
            g_mode = Mode::Starting;
            g_start_port = -1;
            g_go_at = now_ms();
        } else {
            g_notice = "Joining " + found.game.name + "'s lobby...";
            g_target_in_lobby = true;
        }
    }
    // One entry a host, though a broadcast reaches it on every network it is on and the loopback too:
    // its LAN address is kept over 127.0.0.1, which only works on this machine.
    for (Found& known : g_found) {
        if (known.id == found.id && known.game.address != found.game.address) {
            const bool known_loopback = known.game.address.rfind("127.", 0) == 0;
            const bool found_loopback = found.game.address.rfind("127.", 0) == 0;
            if (found_loopback && !known_loopback) {
                known.heard = found.heard;
                known.game.players = found.game.players;
                known.game.playing = found.game.playing;
                known.game.joinable = found.game.joinable;
                known.game.problem = found.game.problem;
                return;
            }
        }
        if (known.id == found.id) {
            known = found;
            return;
        }
    }
    g_found.insert(g_found.begin(), found);
    if (g_found.size() > kMaxGames) {
        g_found.pop_back();  // the one not heard from longest
    }
}

// The host: the session's input delay in frames. A guest's input reaches the host and comes back
// merged: a whole round trip. The session would take it from the round trip at its start, one moment;
// a mobile link went from 97 ms then to 167 ms in play, and every frame past the delay waited. The
// lobby has heard each member's ping the whole time: the delay covers the slowest member's 90th
// percentile ping, plus 15 ms of send tick and frame timing and 30 ms for the jitter beyond it.
// --net-delay given to the lobby wins.
int session_delay() {
    if (conker::net::delay_option() > 0) {
        return conker::net::delay_option();
    }
    int ping = 0;
    for (const Member& m : g_members) {
        std::vector<int> pings = m.pings;
        if (pings.empty()) {
            continue;
        }
        std::sort(pings.begin(), pings.end());
        ping = std::max(ping, pings[std::min(pings.size() - 1, pings.size() * 9 / 10)]);
    }
    return std::clamp(static_cast<int>(std::ceil((ping + 15 + 30) / 33.3)), 2, 15);
}

// Builds the session's command line and starts this game again with it (does not return if it can).
void relaunch_now() {
    std::vector<std::string> args;
    const std::string name = clean_name(read_setting("name"));
    char token[17];
    if (g_start_port == 0) {
        args = {"--net-host", std::to_string(g_port), "--net-players", std::to_string(g_start_players), "--net-name",
                name};
        if (g_map_tried) {
            args.push_back("--net-map");
        }
        const int delay = session_delay();
        args.insert(args.end(), {"--net-delay", std::to_string(delay)});
        std::snprintf(token, sizeof(token), "%016llx", static_cast<unsigned long long>(g_session_token));
        args.insert(args.end(), {"--net-token", token});
    } else {
        args = {"--net-join", address_with_port(g_host_addr, g_host_len), "--net-name", name};
        if (g_start_port > 0) {
            args.push_back("--net-want-port");
            args.push_back(std::to_string(g_start_port + 1));
            std::snprintf(token, sizeof(token), "%016llx", static_cast<unsigned long long>(g_start_token));
            args.insert(args.end(), {"--net-token", token});
        }
    }
    close_sockets();
    std::string line;
    for (size_t i = 0; i < args.size(); i++) {
        line += " " + (i > 0 && args[i - 1] == "--net-token" ? std::string("<token>") : args[i]);
    }
    std::printf("[conker] net: the lobby starts the session: starting again with%s\n", line.c_str());
    conker::net::relaunch(args);
}

void handle_packet(int socket, const uint8_t* data, size_t size, const sockaddr_storage& from, socklen_t from_len) {
    Reader r{data, size};
    const uint32_t magic = r.u32();
    const uint8_t type = r.u8();
    if (!r.ok || magic != (type == kProbe || type == kOffer ? kDiscoveryMagic : kMagic)) {
        return;
    }
    const int64_t now = now_ms();
    // CONKER_NET_TEST_DEAF=1: a hosting lobby ignores its game port (the LAN list still finds it), as
    // a firewall dropping a joining game's replies looks to it.
    static const bool deaf = [] {
        const char* value = std::getenv("CONKER_NET_TEST_DEAF");
        return value != nullptr && value[0] == '1';
    }();
    if (deaf && g_mode == Mode::Hosting && socket == g_socket) {
        return;
    }
    if (type == kProbe && g_mode == Mode::Hosting) {
        // The discovery port is for this network only (it is not mapped on the router).
        if (socket != g_discovery_socket || local_network_address(from)) {
            offer_to(socket, r, size, from, from_len, lobby_offer());
        }
        return;
    }
    if (type == kOffer) {
        // Only answers to this machine's PROBEs: the LAN list's (browsing) and the game being joined's.
        if (socket == g_probe_socket && g_browsing) {
            on_found(from, from_len, r, false);
        } else if (socket == g_socket && g_mode == Mode::Probing) {
            on_found(from, from_len, r, true);
        }
        return;
    }
    if (g_mode == Mode::Hosting) {
        Member* member = nullptr;
        for (Member& m : g_members) {
            if (same_address(m.addr, m.len, from, from_len)) {
                member = &m;
            }
        }
        if (type == kJoin) {
            const uint32_t nonce = r.u32();
            const std::string name = clean_name(r.text(16));
            const std::string pad = known_pad(r.text(40));
            const std::string build = r.text(200);
            const std::string os = r.text(64);
            const uint64_t rom = r.u64();
            const uint64_t cookie = r.u64();
            const uint16_t protocol = r.size - r.at >= 2 ? r.u16() : 0;  // a JOIN before version 5 ends before it
            if (!r.ok) {
                return;
            }
            if (member != nullptr) {
                // A JOIN sent again. It says nothing for the member: it carries no secret, so a sender
                // that forged the member's address kept a gone member's seat. The member's
                // own HEARTBEATs keep it.
                return;
            }
            const std::string ip = address_with_port(from, from_len).substr(0, address_with_port(from, from_len).rfind(':'));
            if (!address_cookie_ok(from, kCookiePurpose, cookie)) {
                // Nothing else for an address that has not shown it gets what is sent there.
                Writer w(kCookie);
                w.u32(nonce);
                w.u64(address_cookie(from, kCookiePurpose));
                send_to(socket, from, from_len, w);
                return;
            }
            std::string problem = mismatch(build, os, rom, protocol);
            if (problem.empty() && std::find(g_refused.begin(), g_refused.end(), ip) != g_refused.end()) {
                problem = "the host removed this machine from the lobby";
            } else if (problem.empty() && g_start_at != 0) {
                problem = "the host is already starting the session";
            } else if (problem.empty() && 1 + static_cast<int>(g_members.size()) >= kMaxPlayers) {
                problem = "the lobby is full (four players)";
            }
            if (!problem.empty()) {
                Writer w(kReject);
                w.u32(nonce);
                w.u64(0);  // not a member: no secret yet
                w.text("Cannot join: " + problem + ".", 300);
                send_to(socket, from, from_len, w);
                g_notice = name + " could not join: " + problem + ".";
                std::printf("[conker] net: lobby: %s turned away: %s\n", address_with_port(from, from_len).c_str(),
                            problem.c_str());
                return;
            }
            Member m;
            m.addr = from;
            m.len = from_len;
            m.nonce = nonce;
            m.secret = random64();  // told to this member in STATE; required in its control packets (J)
            m.name = name;
            m.pad = pad;
            m.last_heard = now;
            g_members.push_back(m);
            g_notice = name + " joined.";
            std::printf("[conker] net: lobby: %s joined from %s\n", name.c_str(), address_with_port(from, from_len).c_str());
            send_state(g_members.back(), now);
            return;
        }
        if (member == nullptr) {
            return;
        }
        // HEARTBEAT, GO and LEAVE carry the member's secret (told it in STATE): a sender who only forged
        // this member's address, without the secret, cannot forge its ready/go state or evict it (J).
        const uint64_t secret = r.u64();
        if (!r.ok || !same_secret(secret, member->secret)) {
            return;
        }
        member->last_heard = now;
        if (type == kHeartbeat) {
            const uint32_t echo = r.u32();
            const uint8_t ready = r.u8();
            const uint16_t ping = r.u16();
            if (r.ok) {
                member->ready = ready != 0;
                member->ping = ping == 0xFFFF ? -1 : ping;
                if (member->ping >= 0 && member->pings.size() < 4096) {
                    member->pings.push_back(member->ping);
                }
                member->echo = echo;
                member->echo_at = now;
            }
        } else if (type == kGo) {
            member->go = true;
        } else if (type == kLeave) {
            g_notice = member->name + " left.";
            std::printf("[conker] net: lobby: %s left\n", member->name.c_str());
            g_members.erase(g_members.begin() + (member - g_members.data()));
        }
        return;
    }
    // Joining: only the host's packets count.
    if (!same_address(from, from_len, g_host_addr, g_host_len)) {
        return;
    }
    // Every packet from the host carries the JOIN's nonce (STATE, START, REJECT, COOKIE) or the member
    // secret the first STATE gave (STATE, START, REJECT and LEAVE after it): a sender that forged the
    // host's address had the broadcast LAN-list nonce and needed nothing for LEAVE.
    const auto from_host = [](uint64_t secret) {
        return g_member_secret == 0 ? secret == 0 : same_secret(secret, g_member_secret);
    };
    if (type == kCookie && r.u32() == g_join_nonce && g_mode == Mode::Probing) {
        const uint64_t cookie = r.u64();
        if (r.ok && cookie != g_join_cookie) {
            g_join_cookie = cookie;
            g_last_probe_to_host = 0;  // the JOIN again at once, with it
        }
        return;
    }
    if (type == kReject && r.u32() == g_join_nonce) {
        const uint64_t secret = r.u64();
        const std::string why = printable(r.text(300), 300);
        if (!r.ok || !from_host(secret)) {
            return;
        }
        g_error = why;
        g_mode = Mode::Idle;
        g_joined_lobby = false;
        return;
    }
    if (type == kState && r.u32() == g_join_nonce && (g_mode == Mode::Probing || g_mode == Mode::Joined)) {
        const uint64_t member_secret = r.u64();  // this machine's secret for control packets (J, N2)
        const uint32_t echo = r.u32();
        const uint16_t held = r.u16();  // how long the host had the echo before this STATE
        std::vector<Player> players(r.u8());
        for (Player& p : players) {
            p.name = clean_name(r.text(16));
            p.pad = known_pad(r.text(40));
            const uint16_t ping = r.u16();
            p.ping = ping == 0xFFFF ? -1 : ping;
            const uint8_t flags = r.u8();
            p.ready = (flags & 1) != 0;
            p.host = (flags & 2) != 0;
            p.you = (flags & 4) != 0;
        }
        const std::string settings = printable(r.text(1000), 1000, true);
        // The first STATE gives the secret; every later one must carry the same (one that changed it
        // made this machine's HEARTBEATs fail the host's check, so the host let it go).
        if (!r.ok || member_secret == 0 || (g_member_secret != 0 && !same_secret(member_secret, g_member_secret))) {
            return;
        }
        if (echo != 0) {
            const int64_t ping = static_cast<int64_t>(static_cast<uint32_t>(now) - echo) - held;
            if (ping >= 0 && ping < 10000) {
                g_ping = static_cast<int>(ping);
            }
        }
        g_member_secret = member_secret;
        if (!players.empty()) {
            players[0].ping = g_ping;  // the host's ping, as seen from here
        }
        for (Player& p : players) {
            if (p.you) {
                p.pad = own_pad();  // this machine's own pad by its name, here only
            }
        }
        g_players = players;
        g_settings = settings;
        g_host_heard = now;
        if (g_mode == Mode::Probing) {
            g_mode = Mode::Joined;
            g_joined_lobby = true;
            g_notice = "In " + (players.empty() ? std::string("the host") : players[0].name) + "'s lobby. Press READY.";
            write_setting("last", g_target_text);
        }
        return;
    }
    if (type == kStart && r.u32() == g_join_nonce && (g_mode == Mode::Joined || g_mode == Mode::Starting)) {
        const uint64_t secret = r.u64();
        const int port = r.u8();
        const int players = r.u8();
        const uint64_t token = r.u64();
        if (!r.ok || g_member_secret == 0 || !from_host(secret) || port < 1 || port >= kMaxPlayers || players < 2 ||
            players > kMaxPlayers) {
            return;
        }
        Writer w(kGo);
        w.u64(g_member_secret);
        send_to(socket, g_host_addr, g_host_len, w);
        if (g_mode != Mode::Starting) {
            g_mode = Mode::Starting;
            g_start_port = port;
            g_start_players = players;
            g_start_token = token;
            g_go_at = now + 500;  // the host starts again first, then answers this machine's HELLO
            g_notice = "The host started the session: starting again as player " + std::to_string(port + 1) + "...";
        }
        return;
    }
    if (type == kLeave && g_mode == Mode::Joined && g_member_secret != 0 && from_host(r.u64()) && r.ok) {
        g_error = "The host closed the lobby.";
        g_mode = Mode::Idle;
        g_joined_lobby = false;
    }
}

void send_probes(int64_t now) {
    if (g_probe_socket < 0) {
        g_probe_socket = open_ipv4_socket(0, false);
    }
    if (g_probe_socket < 0) {
        return;
    }
    g_last_probe = now;
    std::vector<uint32_t> targets = local_addresses().ipv4_broadcasts;
    targets.push_back(INADDR_BROADCAST);
    targets.push_back(INADDR_LOOPBACK);  // games on this machine
    for (uint32_t target : targets) {
        sockaddr_storage to{};
        auto& v4 = reinterpret_cast<sockaddr_in&>(to);
        v4.sin_family = AF_INET;
        v4.sin_addr.s_addr = htonl(target);
        v4.sin_port = htons(kDiscoveryPort);
#if defined(__APPLE__)
        v4.sin_len = sizeof(sockaddr_in);
#endif
        send_probe(g_probe_socket, to, sizeof(sockaddr_in), g_nonce);
    }
}

void tick(int64_t now) {
    // Each error once in the log too: a join that ends on screen only left nothing to go on in a report.
    static std::string logged_error;
    if (g_error != logged_error) {
        logged_error = g_error;
        if (!g_error.empty()) {
            std::printf("[conker] net: lobby: %s\n", g_error.c_str());
        }
    }
    if (g_browsing && now - g_last_probe >= kProbeEveryMs) {
        send_probes(now);
    }
    g_found.erase(std::remove_if(g_found.begin(), g_found.end(), [now](const Found& f) { return now - f.heard > kGameKeptMs; }),
                  g_found.end());
    static int64_t last_tick = 0;
    const bool due = now - last_tick >= kTickMs;
    if (due) {
        last_tick = now;
    }
    if (g_mode == Mode::Hosting) {
        if (due) {
            for (const Member& m : g_members) {
                send_state(m, now);
            }
        }
        for (size_t i = 0; i < g_members.size();) {
            if (now - g_members[i].last_heard > kGoneAfterMs) {
                g_notice = g_members[i].name + " left (not heard from).";
                std::printf("[conker] net: lobby: %s not heard from; gone\n", g_members[i].name.c_str());
                g_members.erase(g_members.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                i++;
            }
        }
        if (g_start_at != 0 && due) {
            bool all_go = true;
            for (size_t i = 0; i < g_members.size(); i++) {
                Writer w(kStart);
                w.u32(g_members[i].nonce);
                w.u64(g_members[i].secret);
                w.u8(static_cast<uint8_t>(i + 1));
                w.u8(static_cast<uint8_t>(g_start_players));
                w.u64(g_session_token);
                send_to(g_socket, g_members[i].addr, g_members[i].len, w);
                all_go = all_go && g_members[i].go;
            }
            if (all_go || now - g_start_at >= kStartWaitMs) {
                g_start_port = 0;
                relaunch_now();
            }
        }
        return;
    }
    if (g_mode == Mode::Probing) {
        if (now - g_last_probe_to_host >= 400) {
            g_last_probe_to_host = now;
            send_probe(g_socket, g_host_addr, g_host_len, g_join_nonce);
            // A JOIN too: a host found by its OFFER takes it; one still starting ignores it.
            Writer join(kJoin);
            join.u32(g_join_nonce);
            join.text(clean_name(read_setting("name")), 16);
            join.text(wire_pad(), 40);
            join.text(conker::net::build_text(), 200);
            join.text(conker::net::os_text(), 64);
            join.u64(conker::net::rom_hash());
            join.u64(g_join_cookie);
            join.u16(kProtocol);
            // Not from the LAN list: it keeps one address a host, maybe not the one being joined.
            if (g_target_in_lobby) {
                send_to(g_socket, g_host_addr, g_host_len, join);
            }
        }
        if (now - g_probe_since > kProbeGiveUpMs) {
            // A game at that address answers the LAN list (its discovery port, another socket and
            // thread) but its game port did not answer: it is there, so the address is right.
            const std::string target = address_with_port(g_host_addr, g_host_len);
            const auto seen = std::find_if(g_found.begin(), g_found.end(),
                                           [&](const Found& f) { return f.game.address == target; });
            if (seen != g_found.end()) {
                g_error = seen->game.name + " is on this network (it is in GAMES ON THIS NETWORK) but did not answer the "
                          "join. This machine's firewall may be blocking the game: on macOS allow it in System "
                          "Settings, Network, Firewall, Options (a build you compiled needs that again after "
                          "every rebuild). Or the host's game may be busy or stuck: try again, or JOIN from the list.";
                g_mode = Mode::Idle;
                return;
            }
            g_error = "No answer from " + g_target_text + ". Check the address or code. The host's game must be in "
                      "its ONLINE lobby (HOST), and its UDP port " +
                      std::to_string(ntohs(g_host_addr.ss_family == AF_INET
                                               ? reinterpret_cast<sockaddr_in&>(g_host_addr).sin_port
                                               : reinterpret_cast<sockaddr_in6&>(g_host_addr).sin6_port)) +
                      " open to this network (see the port forwarding guide, docs/online-play.md). If the host is in a "
                      "match, ask them to go back to the lobby, or wait: a match started without internet play on "
                      "does not answer from outside the LAN.";
            g_mode = Mode::Idle;
        }
        return;
    }
    if (g_mode == Mode::Joined) {
        if (due) {
            Writer w(kHeartbeat);
            w.u64(g_member_secret);
            w.u32(static_cast<uint32_t>(now) | 1);
            w.u8(g_ready ? 1 : 0);
            w.u16(static_cast<uint16_t>(g_ping < 0 ? 0xFFFF : std::min(g_ping, 9999)));
            send_to(g_socket, g_host_addr, g_host_len, w);
        }
        if (now - g_host_heard > kGoneAfterMs) {
            g_error = "The host has not been heard from for " + std::to_string(kGoneAfterMs / 1000) + " s: the lobby is gone.";
            g_mode = Mode::Idle;
            g_joined_lobby = false;
        }
        return;
    }
    if (g_mode == Mode::Starting && g_go_at != 0 && now >= g_go_at) {
        relaunch_now();
        g_error = "Could not start the game again for the session.";
        g_mode = Mode::Idle;
    }
}

void thread_main() {
    uint8_t buffer[1500];
    for (;;) {
        pollfd fds[3];
        int count = 0;
        {
            std::lock_guard lock(g_mutex);
            for (int s : {g_socket, g_probe_socket, g_discovery_socket}) {
                if (s >= 0) {
                    fds[count++] = socket_poll_in(s);
                }
            }
        }
        if (count > 0) {
            socket_poll(fds, static_cast<size_t>(count), 20);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        std::lock_guard lock(g_mutex);
        for (int s : {g_socket, g_probe_socket, g_discovery_socket}) {
            for (int n = 0; s >= 0 && n < 64; n++) {
                sockaddr_storage from{};
                socklen_t from_len = sizeof(from);
                const ssize_t got = socket_receive_from(s, buffer, sizeof(buffer), reinterpret_cast<sockaddr*>(&from), &from_len);
                if (got <= 0) {
                    break;
                }
                if (!sender_allowed(from)) {
                    continue;  // outside the LAN, with internet play off: not looked at, not answered
                }
                handle_packet(s, buffer, static_cast<size_t>(got), from, from_len);
            }
        }
        tick(now_ms());
    }
}

void ensure_thread() {
    if (!g_thread_running) {
        g_thread_running = true;
        g_thread = std::thread(thread_main);
        g_thread.detach();
    }
    if (g_nonce == 0) {
        std::random_device random;
        g_nonce = random() | 1;
    }
}

}  // namespace

std::string name() {
    std::lock_guard lock(g_mutex);
    return clean_name(read_setting("name"));
}

void set_name(const std::string& name) {
    std::lock_guard lock(g_mutex);
    write_setting("name", clean_name(name));
}

std::string last_join() {
    std::lock_guard lock(g_mutex);
    return read_setting("last");
}

bool host(uint16_t port) {
    std::lock_guard lock(g_mutex);
    ensure_thread();
    close_sockets();
    g_members.clear();
    g_refused.clear();
    g_error.clear();
    g_notice.clear();
    g_start_at = 0;
    g_socket = open_socket(port);
    if (g_socket < 0) {
        g_error = "UDP port " + std::to_string(port) + " is taken on this machine (another game hosting?).";
        g_mode = Mode::Idle;
        return false;
    }
    g_port = port;
    g_discovery_socket = open_ipv4_socket(kDiscoveryPort, true);
    g_mode = Mode::Hosting;
    g_map_tried = internet_allowed();
    portmap::open(port);  // not with internet play off: its status says so
    std::printf("[conker] net: lobby: hosting on UDP port %u\n", port);
    return true;
}

void browse() {
    std::lock_guard lock(g_mutex);
    ensure_thread();
    g_browsing = true;
    g_last_probe = 0;
}

void join(const std::string& target) {
    sockaddr_storage addr{};
    socklen_t len = 0;
    std::string why;
    const bool ok = read_join_target(target, kDefaultPort, &addr, &len, &why);  // may ask DNS: not under the lock
    std::lock_guard lock(g_mutex);
    ensure_thread();
    g_error.clear();
    g_notice.clear();
    if (!ok) {
        g_error = "Cannot join \"" + target + "\": " + why + ".";
        return;
    }
    if (!internet_allowed() && !lan_address(addr)) {
        g_error = "Cannot join \"" + target + "\": " + internet_off_message() + ".";
        return;
    }
    if (g_mode == Mode::Hosting) {
        g_error = "Leave the lobby you host first.";
        return;
    }
    if (g_socket < 0) {
        g_socket = open_socket(0);
    }
    g_target_text = target;
    g_host_addr = addr;
    g_host_len = len;
    g_mode = Mode::Probing;
    g_probe_since = now_ms();
    g_target_in_lobby = false;
    g_join_cookie = 0;
    g_member_secret = 0;
    g_join_nonce = static_cast<uint32_t>(random64()) | 1;
    g_last_probe_to_host = 0;
    g_ready = false;
    g_ping = -1;
    g_players.clear();
    g_notice = "Asking " + address_with_port(addr, len) + "...";
    std::printf("[conker] net: lobby: joining %s\n", address_with_port(addr, len).c_str());
}

void set_ready(bool ready) {
    std::lock_guard lock(g_mutex);
    g_ready = ready;
}

void start() {
    std::lock_guard lock(g_mutex);
    if (g_mode != Mode::Hosting || g_members.empty() || g_start_at != 0) {
        return;
    }
    for (const Member& m : g_members) {
        if (!m.ready) {
            g_error = m.name + " is not ready.";
            return;
        }
    }
    g_start_players = 1 + static_cast<int>(g_members.size());
    g_session_token = random64();
    g_start_at = now_ms();
    g_notice = "Starting the session for " + std::to_string(g_start_players) + " players...";
    std::printf("[conker] net: lobby: START for %d players\n", g_start_players);
}

void kick(size_t player) {
    std::lock_guard lock(g_mutex);
    if (g_mode != Mode::Hosting || player == 0 || player > g_members.size() || g_start_at != 0) {
        return;
    }
    const Member m = g_members[player - 1];
    Writer w(kReject);
    w.u32(m.nonce);
    w.u64(m.secret);
    w.text("The host removed you from the lobby.", 300);
    send_to(g_socket, m.addr, m.len, w);
    const std::string address = address_with_port(m.addr, m.len);
    g_refused.push_back(address.substr(0, address.rfind(':')));
    g_members.erase(g_members.begin() + static_cast<std::ptrdiff_t>(player - 1));
    g_notice = m.name + " was removed from the lobby.";
    std::printf("[conker] net: lobby: %s (%s) removed by the host\n", m.name.c_str(), address.c_str());
}

void leave() {
    std::lock_guard lock(g_mutex);
    if (g_mode == Mode::Hosting) {
        for (const Member& m : g_members) {
            Writer w(kLeave);
            w.u64(m.secret);  // host -> member LEAVE carries the member's secret too (N2)
            send_to(g_socket, m.addr, m.len, w);
        }
        portmap::close();
    } else if (g_mode == Mode::Joined) {
        Writer w(kLeave);
        w.u64(g_member_secret);  // member -> host LEAVE carries the secret (J)
        send_to(g_socket, g_host_addr, g_host_len, w);
    }
    close_sockets();
    g_members.clear();
    g_found.clear();
    g_browsing = false;
    g_mode = Mode::Idle;
    g_joined_lobby = false;
    g_error.clear();
    g_notice.clear();
}

View view() {
    std::lock_guard lock(g_mutex);
    View v;
    v.mode = g_mode;
    v.error = g_error;
    v.notice = g_notice;
    v.ready = g_ready;
    v.game_port = g_port;
    for (const Found& f : g_found) {
        v.games.push_back(f.game);
    }
    if (g_mode == Mode::Hosting) {
        v.players = host_players();
        v.settings = settings_summary(conker::frame_input::settings_header());
        v.port = portmap::status();
        v.can_start = !g_members.empty() && g_start_at == 0 &&
                      std::all_of(g_members.begin(), g_members.end(), [](const Member& m) { return m.ready; });
        static int64_t codes_at = 0;
        static std::vector<std::pair<std::string, std::string>> codes;
        static std::string codes_external;
        if (now_ms() - codes_at < 2000 && codes_external == v.port.external) {
            v.codes = codes;
            return v;
        }
        codes_at = now_ms();
        codes_external = v.port.external;
        const LocalAddresses local = local_addresses();
        if (v.port.state == portmap::Status::State::Opened && !v.port.external.empty()) {
            sockaddr_storage outside{};
            socklen_t outside_len;
            std::string why;
            // The router's word: an outside address that is not a public one (a private, loopback or
            // shared one, from a router behind another or a machine posing as the router) makes no
            // Internet code, which would send its joiners elsewhere.
            in_addr external{};
            if (inet_pton(AF_INET, v.port.external.c_str(), &external) == 1 &&
                ipv4_kind(ntohl(external.s_addr)) == AddressKind::Public &&
                read_join_target(v.port.external, g_port, &outside, &outside_len, &why)) {
                v.codes.push_back({"Internet", make_join_code(outside, g_port)});
            }
        }
        for (const sockaddr_storage& a : local.ipv6) {
            if (!internet_allowed()) {
                break;  // a global IPv6 address is the internet
            }
            v.codes.push_back({"IPv6", make_join_code(a, g_port)});
            break;
        }
        for (const sockaddr_storage& a : local.ipv4) {
            v.codes.push_back({"This network (" + address_with_port(a, 0).substr(0, address_with_port(a, 0).rfind(':')) + ")",
                               make_join_code(a, g_port)});
        }
        codes = v.codes;
    } else {
        v.players = g_players;
        v.settings = g_settings;
        v.settings_differ = g_settings.empty() ? "" : settings_differ(g_settings);
    }
    return v;
}

bool answer_probe(int socket, const uint8_t* data, size_t size, const sockaddr_storage& from, socklen_t from_len,
                  const Offer& offer) {
    Reader r{data, size};
    if (r.u32() != kDiscoveryMagic || r.u8() != kProbe) {
        return false;
    }
    offer_to(socket, r, size, from, from_len, offer);
    return true;
}

void serve_discovery(Offer (*offer)()) {
    const int s = open_ipv4_socket(kDiscoveryPort, true);
    if (s < 0) {
        std::printf("[conker] net: no LAN discovery: UDP port %u is not free\n", kDiscoveryPort);
        return;
    }
    std::thread([s, offer] {
        uint8_t buffer[1500];
        for (;;) {
            pollfd fd = socket_poll_in(s);
            socket_poll(&fd, 1, 1000);
            sockaddr_storage from{};
            socklen_t from_len = sizeof(from);
            const ssize_t got = socket_receive_from(s, buffer, sizeof(buffer), reinterpret_cast<sockaddr*>(&from), &from_len);
            if (got > 0 && local_network_address(from) && sender_allowed(from)) {  // this network only
                answer_probe(s, buffer, static_cast<size_t>(got), from, from_len, offer());
            }
        }
    }).detach();
}

}  // namespace conker::net::lobby
