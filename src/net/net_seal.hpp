// Sealed datagrams: the lobby's and the session's packets encrypted and authenticated, so that a machine
// that sees the traffic (public Wi-Fi, a shared network) learns no secret or token from it and cannot
// forge or replay a packet (docs/online-play.md, "Security").
//
// Each host has a key pair (X25519). A guest makes one of its own for each join, and asks the host for
// its public key before anything else:
//
//     ASK      guest -> host    "CKS1", 1, the guest's public key, padded to kAskSize
//     OFFER    host -> guest    "CKS1", 2, a cookie for the guest's address, the host's public key
//                               (smaller than the ASK, so that a forged sender gets nothing bigger back)
//     FIRST    guest -> host    "CKS1", 3, counter, the guest's public key, the cookie, sealed packet
//     SEALED   either way       "CKS1", 4, counter, sealed packet
//
// Both sides take the key shared between their two key pairs (hashed with both public keys) and seal
// every lobby and session packet but the LAN list's PROBE and OFFER with XChaCha20-Poly1305, each
// direction its own nonces (the packet's counter). A counter seen before, or further back than the last
// 64, is refused. A FIRST carries the guest's key with it, so the host keeps nothing for a guest before
// that guest's first packet checks out; its cookie in the clear lets the host refuse a forged sender
// before the work of the key exchange.
//
// A guest that knows the host's key beforehand (a join code holds 40 bits of it; the lobby hands the
// whole key to the session) refuses an OFFER of another, so that a machine between the two cannot
// pose as the host. One that does not (an address typed in, the LAN list) takes the key the host
// offers: the traffic is still sealed against anyone who only sees it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace conker::net::seal {

constexpr uint32_t kMagic = 0x31534B43;  // "CKS1"
enum Kind : uint8_t { kAsk = 1, kOffer = 2, kFirst = 3, kSealed = 4 };
constexpr size_t kAskSize = 160;
// What sealing adds to a packet: magic, kind, counter and tag (and a FIRST's key and cookie).
constexpr size_t kOverhead = 4 + 1 + 8 + 16;
constexpr size_t kFirstOverhead = kOverhead + 32 + 8;

struct KeyPair {
    uint8_t secret[32] = {};
    uint8_t pub[32] = {};
};

// A new key pair, from the system's random numbers.
KeyPair new_key_pair();
// The key pair whose secret key is in hex (64 characters); false if it is not one.
bool key_pair_from_hex(const std::string& hex, KeyPair* pair);
std::string hex(const uint8_t key[32]);
bool key_from_hex(const std::string& hex, uint8_t key[32]);

// The 40 bits of a public key a join code holds.
uint64_t fingerprint(const uint8_t pub[32]);

// What a guest knows of the host's key before it asks: nothing, a join code's fingerprint, or the
// whole key (the lobby's, handed to the session).
struct Expected {
    bool has_fingerprint = false;
    uint64_t fingerprint = 0;
    bool has_key = false;
    uint8_t key[32] = {};
    bool matches(const uint8_t pub[32]) const;
};
// "" (nothing), 10 hex characters (a fingerprint) or 64 (a key); false if it is none of those.
bool expected_from_hex(const std::string& hex, Expected* expected);

// One guest's link with the host, on either side.
struct Channel {
    bool ready = false;
    bool host_side = false;
    uint8_t key[32] = {};
    uint8_t guest_pub[32] = {};  // which guest key pair it is for
    uint64_t sent = 0;           // the last counter sent
    uint64_t highest = 0;        // the highest counter taken in
    uint64_t seen = 0;           // a bit each for the 64 counters up to it
};

// The kind of a datagram, or 0 for one that is not sealed (an older build's, the LAN list's).
uint8_t kind_of(const uint8_t* data, size_t size);

// --- the guest -------------------------------------------------------------------------------------

// An ASK with this key pair's public key. The size written.
size_t write_ask(const KeyPair& guest, uint8_t* out, size_t capacity);
// An OFFER's cookie and host key; false if it is not an OFFER.
bool read_offer(const uint8_t* data, size_t size, uint64_t* cookie, uint8_t host_pub[32]);
// The link with the host that offered `host_pub`.
Channel guest_channel(const KeyPair& guest, const uint8_t host_pub[32]);
// A FIRST: the packet sealed, with the guest's key and the cookie. 0 if it does not fit.
size_t seal_first(Channel& channel, uint64_t cookie, const uint8_t* plain, size_t size, uint8_t* out, size_t capacity);

// --- the host --------------------------------------------------------------------------------------

// Whether a datagram is an ASK padded as it must be.
bool is_ask(const uint8_t* data, size_t size);
// An OFFER of the host's key with the guest's cookie. The size written.
size_t write_offer(const KeyPair& host, uint64_t cookie, uint8_t* out, size_t capacity);
// A FIRST's cookie (in the clear), to check before opening it; false if it is not a FIRST.
bool first_cookie(const uint8_t* data, size_t size, uint64_t* cookie);
// A FIRST's guest key (in the clear), to tell whether it is a known guest's.
bool first_guest(const uint8_t* data, size_t size, uint8_t guest_pub[32]);
// Opens a FIRST: the link with its guest in *channel, the packet in *plain (at most `capacity`
// bytes), its size in *plain_size. False if it is not one or does not check out.
bool open_first(const KeyPair& host, const uint8_t* data, size_t size, Channel* channel, uint8_t* plain,
                size_t capacity, size_t* plain_size);

// --- both ------------------------------------------------------------------------------------------

// A SEALED packet. 0 if it does not fit.
size_t seal(Channel& channel, const uint8_t* plain, size_t size, uint8_t* out, size_t capacity);
// Opens a SEALED packet, or a FIRST, on its link. False if it does not check out or was seen before.
bool open(Channel& channel, const uint8_t* data, size_t size, uint8_t* plain, size_t capacity, size_t* plain_size);

}  // namespace conker::net::seal
