// The socket calls online play makes, on POSIX and on Windows (Winsock).
//
// A socket is kept as an int on both: Winsock's SOCKET handles are small numbers in practice, and
// INVALID_SOCKET becomes -1, as a failed socket() is on POSIX.
#pragma once

#include <cstddef>
#include <string>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
using ssize_t = std::ptrdiff_t;
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#endif

namespace conker::net {

// Once, before the first socket (WSAStartup on Windows). False if sockets cannot be used.
bool sockets_init();

void socket_close(int s);

// Non-blocking and not handed on to the game when it starts again (relaunch). On Windows, a UDP
// socket also stops reporting a peer's closed port as an error on the next receive.
void socket_prepare(int s);

int socket_set_option(int s, int level, int name, int value);

// A pollfd waiting for `s` to have a datagram.
pollfd socket_poll_in(int s);

// poll() on the sockets in `fds`.
int socket_poll(pollfd* fds, size_t count, int timeout_ms);

ssize_t socket_send_to(int s, const void* data, size_t size, const sockaddr* to, socklen_t to_len);

// Never waits: 0 or less when nothing is there.
ssize_t socket_receive_from(int s, void* buffer, size_t size, sockaddr* from, socklen_t* from_len);

// The last socket call's error, as text.
std::string socket_error_text();

} // namespace conker::net
