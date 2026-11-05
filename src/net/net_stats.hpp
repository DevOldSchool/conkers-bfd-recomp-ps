// Online play's traffic, counted (CONKER_NET_STATS): to measure what each change to the wire saves.
//
// CONKER_NET_STATS=1 prints a report every 10 s while a session runs (=N: every N seconds), and one
// when the session ends. Each report covers the session so far:
//
//   - every packet type sent and received: packets, bytes on the wire (the sealed datagram plus its
//     IP and UDP headers), the average packet;
//   - the INPUT and MERGED packets sent, split into what their bytes carry (headers, times, frame
//     numbers, records, hashes, drops), and how many records each carried (the resends);
//   - all of it per game frame played, the figure to compare between runs: a run that plays slower
//     sends fewer packets a second for the same game.
//
// Off, every call returns at once.
#pragma once

#include <cstddef>
#include <cstdint>

namespace conker::net::stats {

// What the bytes of an INPUT or MERGED packet carry (stats::part).
enum Part : uint8_t {
    kPartIp,       // the IP and UDP headers
    kPartSeal,     // sealing's magic, kind, counter and tag
    kPartHeader,   // magic, type, session, secret (and a guest's port)
    kPartTimes,    // the send time and echo, for the round trip
    kPartFrames,   // acknowledgements, frame numbers, counts, the host's waiting-for ports
    kPartRecords,  // the input records: masks and ports
    kPartHashes,   // the game memory hashes
    kPartDrops,    // the drops
    kPartCount
};

bool enabled();

// Whether this machine hosts (for the report's label); set when a session starts.
void begin_session(bool host);

// A datagram of `type` (net_play's Type; 0 for one sent before sealing) sent or received, `wire`
// bytes on the wire (stats::wire_bytes).
void sent(uint8_t type, size_t wire);
void received(uint8_t type, size_t wire);

// The bytes on the wire of a datagram of `size` bytes: IP and UDP headers added (IPv4: 28, IPv6: 48).
size_t wire_bytes(size_t size, bool ipv6);

// An INPUT or MERGED packet sent: `bytes` of it went to `part`.
void part(bool merged, Part part, size_t bytes);

// An INPUT or MERGED packet sent carried `count` records, `fresh` of them never sent before.
void records(bool merged, uint32_t count, uint32_t fresh);

// A game frame played in the session.
void frame_played();

// Prints a report when one is due (the network thread calls this on its tick).
void tick();

// Prints the session's last report (when the session ends).
void report();

}  // namespace conker::net::stats
