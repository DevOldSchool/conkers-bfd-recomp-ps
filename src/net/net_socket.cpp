#include "net/net_socket.hpp"

#include <cerrno>
#include <cstring>

#if defined(_WIN32)
#include <mswsock.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace conker::net {

bool sockets_init() {
#if defined(_WIN32)
    static const bool ready = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return ready;
#else
    return true;
#endif
}

void socket_close(int s) {
#if defined(_WIN32)
    closesocket(static_cast<SOCKET>(s));
#else
    ::close(s);
#endif
}

void socket_prepare(int s) {
#if defined(_WIN32)
    const SOCKET socket = static_cast<SOCKET>(s);
    u_long on = 1;
    ioctlsocket(socket, FIONBIO, &on);
    SetHandleInformation(reinterpret_cast<HANDLE>(socket), HANDLE_FLAG_INHERIT, 0);
    // Without this, an ICMP port unreachable for an earlier datagram (a peer that quit) fails the next
    // recvfrom with WSAECONNRESET, as if the socket itself were broken.
    BOOL report = FALSE;
    DWORD returned = 0;
    WSAIoctl(socket, SIO_UDP_CONNRESET, &report, sizeof(report), nullptr, 0, &returned, nullptr, nullptr);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
    fcntl(s, F_SETFD, FD_CLOEXEC);
#endif
}

int socket_set_option(int s, int level, int name, int value) {
#if defined(_WIN32)
    return setsockopt(static_cast<SOCKET>(s), level, name, reinterpret_cast<const char*>(&value), sizeof(value));
#else
    return setsockopt(s, level, name, &value, sizeof(value));
#endif
}

pollfd socket_poll_in(int s) {
    pollfd fd{};
#if defined(_WIN32)
    fd.fd = static_cast<SOCKET>(s);
#else
    fd.fd = s;
#endif
    fd.events = POLLIN;
    return fd;
}

int socket_poll(pollfd* fds, size_t count, int timeout_ms) {
#if defined(_WIN32)
    return WSAPoll(fds, static_cast<ULONG>(count), timeout_ms);
#else
    return poll(fds, static_cast<nfds_t>(count), timeout_ms);
#endif
}

ssize_t socket_send_to(int s, const void* data, size_t size, const sockaddr* to, socklen_t to_len) {
#if defined(_WIN32)
    return sendto(static_cast<SOCKET>(s), static_cast<const char*>(data), static_cast<int>(size), 0, to, to_len);
#else
    return sendto(s, data, size, 0, to, to_len);
#endif
}

ssize_t socket_receive_from(int s, void* buffer, size_t size, sockaddr* from, socklen_t* from_len) {
#if defined(_WIN32)
    // Every socket here went through socket_prepare, so this does not wait.
    return recvfrom(static_cast<SOCKET>(s), static_cast<char*>(buffer), static_cast<int>(size), 0, from, from_len);
#else
    return recvfrom(s, buffer, size, MSG_DONTWAIT, from, from_len);
#endif
}

std::string socket_error_text() {
#if defined(_WIN32)
    const int error = WSAGetLastError();
    char text[256] = {};
    if (FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, static_cast<DWORD>(error), 0, text,
                       sizeof(text), nullptr) == 0) {
        return "Winsock error " + std::to_string(error);
    }
    std::string message = text;
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r' || message.back() == '.')) {
        message.pop_back();
    }
    return message;
#else
    return std::strerror(errno);
#endif
}

} // namespace conker::net
