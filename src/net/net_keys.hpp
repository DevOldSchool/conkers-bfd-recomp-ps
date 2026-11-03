// The keys this machine keeps for online play (net_seal.hpp), in conker-recomp-data/net/:
//
//     host_key       this machine's own key as a host, made the first time it hosts and kept, so that
//                    its join codes and key check stay the same from one HOST to the next. Secret:
//                    whoever has it can pose as this machine's lobby.
//     known_hosts    the key of every host this machine has joined, by address ("address:port key"
//                    lines), so that a host whose key has changed since is not taken without a word
//
// A key check is 40 bits of a host's key in 8 characters ("7QX4-M2KD"): the host's lobby shows its
// own, a guest's lobby the one of the host it reached. The same on both screens means no machine
// between them is posing as the host.
#pragma once

#include <cstdint>
#include <string>

#include "net/net_seal.hpp"

namespace conker::net::keys {

// This machine's key as a host (made and kept the first time). CONKER_NET_TEST_HOST_KEY=<64 hex> gives
// it that secret key instead, so that a test's join codes are known beforehand.
seal::KeyPair host_key();

std::string key_check(const uint8_t pub[32]);

// The key this machine saw last at `address` ("10.0.0.2:37800"); false if it never joined it.
bool known_host(const std::string& address, uint8_t pub[32]);
void remember_host(const std::string& address, const uint8_t pub[32]);

}  // namespace conker::net::keys
