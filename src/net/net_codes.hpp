// Join codes and the addresses behind them.
//
// A join code is a host's address, UDP port and 40 bits of its key (net_seal.hpp: a guest that joins
// by code refuses a machine posing as the host) written in Crockford's base 32 (digits and capitals
// without I, L, O and U), with a check byte so that a mistyped code is refused rather than sent
// somewhere else: 20 characters for IPv4 ("K3F9-2QXA-7M0C-4D8R-WZ1N"), 39 for IPv6. Lower case,
// spaces and dashes are ignored when reading one, and O, I and L read as 0, 1 and 1. A code is no
// secret: the address every guest learns anyway when it connects, and a public key's fingerprint.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "net/net_socket.hpp"

namespace conker::net {

// The code for an address (AF_INET or AF_INET6 in addr), port and key fingerprint
// (seal::fingerprint); "" for another family.
std::string make_join_code(const sockaddr_storage& addr, uint16_t port, uint64_t fingerprint);

// Reads a code back, with its key fingerprint. False, with why ("a character that is not in codes",
// "the code does not check out (a character mistyped?)", "the wrong length"), if it is not one.
bool read_join_code(const std::string& code, sockaddr_storage* addr, socklen_t* len, std::string* why,
                    uint64_t* fingerprint = nullptr);

// What JOIN's text field holds: a join code, or an address with or without a port ("10.0.0.2",
// "10.0.0.2:37800", "[2001:db8::1]:37800", "host.example"). False, with why, if it is neither. A
// code's key fingerprint goes in *fingerprint, kNoFingerprint for an address.
constexpr uint64_t kNoFingerprint = ~uint64_t{0};
bool read_join_target(const std::string& text, uint16_t default_port, sockaddr_storage* addr, socklen_t* len,
                      std::string* why, uint64_t* fingerprint = nullptr);

// "10.0.0.2:37800" or "[2001:db8::1]:37800", for messages and the --net-join command line.
std::string address_with_port(const sockaddr_storage& addr, socklen_t len);

// The kind of an IPv4 address, for telling a host what its router's outside address means.
enum class AddressKind { Public, Private, CarrierGradeNat, Loopback, LinkLocal, Other };
AddressKind ipv4_kind(uint32_t address_host_order);

// This machine's own addresses, for the lobby: IPv4 on the local network (private), and global
// IPv6 ones (2000::/3, which usually need no forwarding, though many routers' firewalls block
// them coming in). Loopback and link-local ones are left out.
struct LocalAddresses {
    std::vector<sockaddr_storage> ipv4;
    std::vector<sockaddr_storage> ipv6;
    std::vector<uint32_t> ipv4_broadcasts;  // each IPv4 network's broadcast address, host order
};
LocalAddresses local_addresses();

// Whether an address is on a network of this machine's kind (loopback, private, link-local, carrier-
// grade NAT, IPv6 unique-local or link-local): the LAN list's discovery port answers only those.
bool local_network_address(const sockaddr_storage& addr);

// --- LAN only, with --net-lan-only (docs/online-play.md) -------------------------------------------
//
// With the switch, every socket of the session, the lobby and the LAN list drops a datagram from
// outside the LAN before anything else looks at it, a guest joins only a LAN address, and nothing is
// mapped on the router.

// Whether an address is on the LAN: loopback, private (10/8, 172.16/12, 192.168/16), link-local
// (169.254/16, fe80::/10) or IPv6 unique-local (fc00::/7), as IPv4 or IPv4-mapped. Carrier-grade NAT
// (100.64/10, which overlay networks such as Tailscale use) is the internet here.
bool lan_address(const sockaddr_storage& addr);

// Off with --net-lan-only: set once from the command line, before any socket.
void set_internet_allowed(bool allowed);
bool internet_allowed();

// Whether a datagram from `from` may be looked at: internet play on, or a LAN sender.
// CONKER_NET_TEST_INTERNET_PORT=<n> makes a sender whose source port is n count as one on the
// internet (a crafted packet from 127.0.0.1 standing in for a stranger: docs/diagnostics.md).
bool sender_allowed(const sockaddr_storage& from);

// What a guest is told when it is pointed at an address outside the LAN.
std::string internet_off_message();

// --- checking who a datagram came from -------------------------------------------------------------
//
// A UDP sender's address can be forged. Before a game sends anything bigger than what it got to an
// address, or keeps a place for it, the sender proves it gets what is sent there: the first answer is
// a small cookie, a keyed hash of its address, which it sends back.

// 64 random bits (never 0).
uint64_t random64();

// SipHash-2-4 of data under a 128-bit key.
uint64_t keyed_hash(const uint64_t key[2], const void* data, size_t size);

// The cookie for a sender's address (and port) for `purpose`, under a key of this run's. One made in
// the last 30 to 60 s checks out.
uint64_t address_cookie(const sockaddr_storage& addr, uint32_t purpose);
bool address_cookie_ok(const sockaddr_storage& addr, uint32_t purpose, uint64_t cookie);

// Whether two secret words (a cookie, a peer's secret, a session token) are the same, in a time that
// does not depend on where they differ.
inline bool same_secret(uint64_t a, uint64_t b) {
    volatile uint64_t difference = a ^ b;
    return difference == 0;
}

// Text from another machine as it may be shown or logged: printable ASCII only (newlines too where
// `newlines`), at most `max` characters.
std::string printable(const std::string& text, size_t max, bool newlines = false);

// Whether to log one more machine turned away: at most a few a minute, so that a machine that keeps
// asking (past the cookie, which any machine on the LAN gets) cannot grow last-run.log without end.
// The first one logged after a quiet spell says how many were left out.
bool log_refusal();

}  // namespace conker::net
