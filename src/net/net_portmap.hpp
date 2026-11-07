// Opening a host's UDP port on its router: UPnP IGD through
// miniupnpc, then NAT-PMP through libnatpmp (which PCP routers answer too). Done on a thread of its
// own, since asking a router takes a few seconds; the mapping is leased for an hour and renewed every
// half hour, and removed when the game exits (close()). A hosting game that starts again into the
// session (the lobby's START) maps the port again, which only renews it.
//
// For testing without touching a real router: CONKER_NET_PORTMAP=0 turns it off;
// CONKER_NET_UPNP_URL=<root description URL> asks that device instead of discovering one, and
// CONKER_NET_NATPMP_GATEWAY=<IPv4 address> that gateway; with either set, only the methods set are
// tried (tools/fake_gateway.py answers both on this machine).
#pragma once

#include <cstdint>
#include <string>

namespace conker::net::portmap {

struct Status {
    enum class State { Off, Working, Opened, Failed };
    State state = State::Off;
    std::string method;         // "UPnP" or "NAT-PMP" once opened
    std::string external;       // the router's outside address, if it said ("" if not)
    std::string message;        // a sentence for the lobby: what happened, and what to do if it failed
};

// Starts opening `port` (UDP, the same number outside and in). Does nothing if already started.
void open(uint16_t port);

// What has happened so far.
Status status();

// Removes the mapping, if one was made (waits up to about 2 s for the router). Safe to call more
// than once; also run at exit once open() was called.
void close();

}  // namespace conker::net::portmap
