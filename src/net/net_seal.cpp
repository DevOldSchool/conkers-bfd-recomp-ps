// Sealed datagrams; see net_seal.hpp.
#include "net/net_seal.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#else
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/random.h>
#endif
#endif

#include "monocypher.h"

namespace conker::net::seal {

namespace {

constexpr size_t kHeader = 4 + 1 + 8;  // magic, kind, counter: the sealed packet's associated data

void put_u32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
void put_u64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
uint32_t get_u32(const uint8_t* p) {
    uint32_t v = 0;
    for (int i = 3; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}
uint64_t get_u64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

void system_random(uint8_t* out, size_t size) {
#if defined(_WIN32)
    if (BCryptGenRandom(nullptr, out, static_cast<ULONG>(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0) {
        return;
    }
#elif CONKER_PS5
    // The console's libc has no getentropy: FreeBSD's arc4random, seeded by the kernel.
    arc4random_buf(out, size);
    return;
#else
    bool ok = true;
    for (size_t at = 0; at < size && ok; at += 256) {
        ok = getentropy(out + at, size - at < 256 ? size - at : 256) == 0;
    }
    if (ok) {
        return;
    }
#endif
    // A key from anything less would be guessable: better no online play than that.
    std::fprintf(stderr, "[conker] net: the system gave no random numbers for a key\n");
    std::abort();
}

// The key both sides take: the X25519 shared secret hashed with both public keys.
void shared_key(const uint8_t own_secret[32], const uint8_t their_pub[32], const uint8_t guest_pub[32],
                const uint8_t host_pub[32], uint8_t key[32]) {
    uint8_t message[16 + 32 * 3];
    std::memcpy(message, "conker seal v1\0\0", 16);
    crypto_x25519(message + 16, own_secret, their_pub);
    std::memcpy(message + 48, guest_pub, 32);
    std::memcpy(message + 80, host_pub, 32);
    crypto_blake2b(key, 32, message, sizeof(message));
    crypto_wipe(message, sizeof(message));
}

// Each direction its own nonces: the counter, and which side sent it.
void nonce_for(uint64_t counter, bool from_host, uint8_t nonce[24]) {
    std::memset(nonce, 0, 24);
    put_u64(nonce, counter);
    nonce[8] = from_host ? 1 : 0;
}

bool sealed_header(const uint8_t* data, size_t size, uint8_t kind, size_t extra) {
    return size >= kOverhead + extra && get_u32(data) == kMagic && data[4] == kind;
}

// Whether a counter is new to the link (and not too far back to tell).
bool fresh(const Channel& channel, uint64_t counter) {
    if (counter == 0) {
        return false;
    }
    if (counter > channel.highest) {
        return true;
    }
    const uint64_t back = channel.highest - counter;
    return back < 64 && (channel.seen & (uint64_t{1} << back)) == 0;
}

void mark(Channel& channel, uint64_t counter) {
    if (counter > channel.highest) {
        const uint64_t ahead = counter - channel.highest;
        channel.seen = ahead >= 64 ? 0 : channel.seen << ahead;
        channel.highest = counter;
    }
    channel.seen |= uint64_t{1} << (channel.highest - counter);
}

// Opens the sealed bytes after `at`: the header and anything in the clear before `at` are its
// associated data.
bool open_at(Channel& channel, const uint8_t* data, size_t size, size_t at, uint8_t* plain, size_t capacity,
             size_t* plain_size) {
    if (size < at + 16) {
        return false;
    }
    const uint64_t counter = get_u64(data + 5);
    const size_t length = size - at - 16;
    if (length > capacity || !fresh(channel, counter)) {
        return false;
    }
    uint8_t nonce[24];
    nonce_for(counter, !channel.host_side, nonce);
    if (crypto_aead_unlock(plain, data + size - 16, channel.key, nonce, data, at, data + at, length) != 0) {
        return false;
    }
    mark(channel, counter);
    *plain_size = length;
    return true;
}

size_t seal_at(Channel& channel, uint8_t kind, const uint8_t* clear, size_t clear_size, const uint8_t* plain,
               size_t size, uint8_t* out, size_t capacity) {
    const size_t at = kHeader + clear_size;
    if (!channel.ready || at + size + 16 > capacity) {
        return 0;
    }
    const uint64_t counter = ++channel.sent;
    put_u32(out, kMagic);
    out[4] = kind;
    put_u64(out + 5, counter);
    if (clear_size > 0) {
        std::memcpy(out + kHeader, clear, clear_size);
    }
    uint8_t nonce[24];
    nonce_for(counter, channel.host_side, nonce);
    crypto_aead_lock(out + at, out + at + size, channel.key, nonce, out, at, plain, size);
    return at + size + 16;
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

KeyPair new_key_pair() {
    KeyPair pair;
    system_random(pair.secret, sizeof(pair.secret));
    crypto_x25519_public_key(pair.pub, pair.secret);
    return pair;
}

bool key_from_hex(const std::string& text, uint8_t key[32]) {
    if (text.size() != 64) {
        return false;
    }
    for (size_t i = 0; i < 32; i++) {
        const int hi = hex_value(text[2 * i]);
        const int lo = hex_value(text[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        key[i] = static_cast<uint8_t>(hi << 4 | lo);
    }
    return true;
}

bool key_pair_from_hex(const std::string& text, KeyPair* pair) {
    if (!key_from_hex(text, pair->secret)) {
        return false;
    }
    crypto_x25519_public_key(pair->pub, pair->secret);
    return true;
}

std::string hex(const uint8_t key[32]) {
    static const char kDigits[] = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < 32; i++) {
        out += kDigits[key[i] >> 4];
        out += kDigits[key[i] & 15];
    }
    return out;
}

uint64_t fingerprint(const uint8_t pub[32]) {
    uint8_t hash[8];
    crypto_blake2b(hash, sizeof(hash), pub, 32);
    return get_u64(hash) & ((uint64_t{1} << 40) - 1);
}

bool Expected::matches(const uint8_t pub[32]) const {
    if (has_key) {
        return crypto_verify32(key, pub) == 0;
    }
    return !has_fingerprint || fingerprint == seal::fingerprint(pub);
}

bool expected_from_hex(const std::string& text, Expected* expected) {
    *expected = Expected{};
    if (text.empty()) {
        return true;
    }
    if (text.size() == 64) {
        expected->has_key = key_from_hex(text, expected->key);
        return expected->has_key;
    }
    if (text.size() != 10) {
        return false;
    }
    uint64_t value = 0;
    for (char c : text) {
        const int digit = hex_value(c);
        if (digit < 0) {
            return false;
        }
        value = value << 4 | static_cast<uint64_t>(digit);
    }
    expected->has_fingerprint = true;
    expected->fingerprint = value;
    return true;
}

uint8_t kind_of(const uint8_t* data, size_t size) {
    return size >= 5 && get_u32(data) == kMagic ? data[4] : 0;
}

size_t write_ask(const KeyPair& guest, uint8_t* out, size_t capacity) {
    if (capacity < kAskSize) {
        return 0;
    }
    std::memset(out, 0, kAskSize);
    put_u32(out, kMagic);
    out[4] = kAsk;
    std::memcpy(out + 5, guest.pub, 32);
    return kAskSize;
}

bool read_offer(const uint8_t* data, size_t size, uint64_t* cookie, uint8_t host_pub[32]) {
    if (size != 4 + 1 + 8 + 32 || get_u32(data) != kMagic || data[4] != kOffer) {
        return false;
    }
    *cookie = get_u64(data + 5);
    std::memcpy(host_pub, data + 13, 32);
    return true;
}

Channel guest_channel(const KeyPair& guest, const uint8_t host_pub[32]) {
    Channel channel;
    shared_key(guest.secret, host_pub, guest.pub, host_pub, channel.key);
    std::memcpy(channel.guest_pub, guest.pub, 32);
    channel.ready = true;
    return channel;
}

size_t seal_first(Channel& channel, uint64_t cookie, const uint8_t* plain, size_t size, uint8_t* out, size_t capacity) {
    uint8_t clear[40];
    std::memcpy(clear, channel.guest_pub, 32);
    put_u64(clear + 32, cookie);
    return seal_at(channel, kFirst, clear, sizeof(clear), plain, size, out, capacity);
}

bool is_ask(const uint8_t* data, size_t size) {
    return size >= kAskSize && get_u32(data) == kMagic && data[4] == kAsk;
}

size_t write_offer(const KeyPair& host, uint64_t cookie, uint8_t* out, size_t capacity) {
    constexpr size_t kSize = 4 + 1 + 8 + 32;
    static_assert(kSize <= kAskSize, "an OFFER is never bigger than its ASK");
    if (capacity < kSize) {
        return 0;
    }
    put_u32(out, kMagic);
    out[4] = kOffer;
    put_u64(out + 5, cookie);
    std::memcpy(out + 13, host.pub, 32);
    return kSize;
}

bool first_guest(const uint8_t* data, size_t size, uint8_t guest_pub[32]) {
    if (!sealed_header(data, size, kFirst, 40)) {
        return false;
    }
    std::memcpy(guest_pub, data + kHeader, 32);
    return true;
}

bool first_cookie(const uint8_t* data, size_t size, uint64_t* cookie) {
    if (!sealed_header(data, size, kFirst, 40)) {
        return false;
    }
    *cookie = get_u64(data + kHeader + 32);
    return true;
}

bool open_first(const KeyPair& host, const uint8_t* data, size_t size, Channel* channel, uint8_t* plain,
                size_t capacity, size_t* plain_size) {
    if (!sealed_header(data, size, kFirst, 40)) {
        return false;
    }
    Channel fresh_channel;
    fresh_channel.host_side = true;
    std::memcpy(fresh_channel.guest_pub, data + kHeader, 32);
    shared_key(host.secret, fresh_channel.guest_pub, fresh_channel.guest_pub, host.pub, fresh_channel.key);
    fresh_channel.ready = true;
    // The host takes a new link for every FIRST of a guest (its HELLO sent again after a COOKIE or a
    // REJECT), all with the same key: its counters go on from the clock's microseconds, so that none
    // is sent twice under that key (no link sends a packet a microsecond).
    fresh_channel.sent = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                                   std::chrono::system_clock::now().time_since_epoch())
                                                   .count());
    if (!open_at(fresh_channel, data, size, kHeader + 40, plain, capacity, plain_size)) {
        crypto_wipe(fresh_channel.key, sizeof(fresh_channel.key));
        return false;
    }
    *channel = fresh_channel;
    return true;
}

size_t seal(Channel& channel, const uint8_t* plain, size_t size, uint8_t* out, size_t capacity) {
    return seal_at(channel, kSealed, nullptr, 0, plain, size, out, capacity);
}

bool open(Channel& channel, const uint8_t* data, size_t size, uint8_t* plain, size_t capacity, size_t* plain_size) {
    if (!channel.ready || size < kOverhead) {
        return false;
    }
    if (sealed_header(data, size, kSealed, 0)) {
        return open_at(channel, data, size, kHeader, plain, capacity, plain_size);
    }
    // A FIRST again from a guest the host has a link with (its HELLO resent): the same guest key only.
    if (sealed_header(data, size, kFirst, 40) && crypto_verify32(channel.guest_pub, data + kHeader) == 0) {
        return open_at(channel, data, size, kHeader + 40, plain, capacity, plain_size);
    }
    return false;
}

}  // namespace conker::net::seal
