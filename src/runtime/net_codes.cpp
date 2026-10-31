// Join codes; see net_codes.hpp.
#include "runtime/net_codes.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cctype>
#include <chrono>
#include <cstring>
#include <mutex>
#include <random>

#if defined(_WIN32)
#include <iphlpapi.h>
#else
#include <ifaddrs.h>
#include <net/if.h>
#endif

namespace conker::net {

namespace {

constexpr char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
constexpr size_t kIpv4Bytes = 4 + 2 + 1;   // address, port, check
constexpr size_t kIpv6Bytes = 16 + 2 + 1;
constexpr size_t chars_for(size_t bytes) { return (bytes * 8 + 4) / 5; }

// CRC-8 (polynomial 0x07) of the address and port, turned so that all zeros is not a valid code.
uint8_t check_byte(const uint8_t* data, size_t size) {
    uint8_t crc = 0;
    for (size_t i = 0; i < size; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = static_cast<uint8_t>((crc & 0x80) != 0 ? (crc << 1) ^ 0x07 : crc << 1);
        }
    }
    return crc ^ 0xA5;
}

std::string encode(const std::vector<uint8_t>& bytes) {
    std::string out;
    uint32_t buffer = 0;
    int bits = 0;
    for (uint8_t byte : bytes) {
        buffer = (buffer << 8) | byte;
        bits += 8;
        while (bits >= 5) {
            out += kAlphabet[(buffer >> (bits - 5)) & 31];
            bits -= 5;
        }
    }
    if (bits > 0) {
        out += kAlphabet[(buffer << (5 - bits)) & 31];
    }
    std::string grouped;
    for (size_t i = 0; i < out.size(); i++) {
        if (i > 0 && i % 4 == 0) {
            grouped += '-';
        }
        grouped += out[i];
    }
    return grouped;
}

int value_of(char c) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (c == 'O') {
        c = '0';
    } else if (c == 'I' || c == 'L') {
        c = '1';
    }
    const char* at = std::strchr(kAlphabet, c);
    return at != nullptr && c != '\0' ? static_cast<int>(at - kAlphabet) : -1;
}

// The code's characters without separators, or "" if one is not a code character.
std::string code_characters(const std::string& code) {
    std::string out;
    for (char c : code) {
        if (c == '-' || c == ' ' || c == '\t') {
            continue;
        }
        if (value_of(c) < 0) {
            return "";
        }
        out += c;
    }
    return out;
}

}  // namespace

std::string make_join_code(const sockaddr_storage& addr, uint16_t port) {
    std::vector<uint8_t> bytes;
    if (addr.ss_family == AF_INET) {
        const auto& v4 = reinterpret_cast<const sockaddr_in&>(addr);
        const auto* a = reinterpret_cast<const uint8_t*>(&v4.sin_addr);
        bytes.assign(a, a + 4);
    } else if (addr.ss_family == AF_INET6) {
        const auto& v6 = reinterpret_cast<const sockaddr_in6&>(addr);
        if (IN6_IS_ADDR_V4MAPPED(&v6.sin6_addr)) {
            bytes.assign(v6.sin6_addr.s6_addr + 12, v6.sin6_addr.s6_addr + 16);
        } else {
            bytes.assign(v6.sin6_addr.s6_addr, v6.sin6_addr.s6_addr + 16);
        }
    } else {
        return "";
    }
    bytes.push_back(static_cast<uint8_t>(port >> 8));
    bytes.push_back(static_cast<uint8_t>(port));
    bytes.push_back(check_byte(bytes.data(), bytes.size()));
    return encode(bytes);
}

// Whether an address can be a host's: not unspecified, broadcast or multicast (a code or address
// someone hands round must not send this machine's datagrams to a whole network).
bool host_address(const sockaddr_storage& addr) {
    if (addr.ss_family == AF_INET) {
        const uint32_t a = ntohl(reinterpret_cast<const sockaddr_in&>(addr).sin_addr.s_addr);
        return a != 0 && (a >> 28) < 14;  // 224.0.0.0 and up: multicast, reserved, 255.255.255.255
    }
    if (addr.ss_family == AF_INET6) {
        const in6_addr& a = reinterpret_cast<const sockaddr_in6&>(addr).sin6_addr;
        return !IN6_IS_ADDR_UNSPECIFIED(&a) && !IN6_IS_ADDR_MULTICAST(&a);
    }
    return false;
}

bool read_join_code(const std::string& code, sockaddr_storage* addr, socklen_t* len, std::string* why) {
    const std::string chars = code_characters(code);
    if (chars.empty()) {
        *why = "it has a character that is not in join codes";
        return false;
    }
    size_t size;
    if (chars.size() == chars_for(kIpv4Bytes)) {
        size = kIpv4Bytes;
    } else if (chars.size() == chars_for(kIpv6Bytes)) {
        size = kIpv6Bytes;
    } else {
        *why = "a join code has 12 characters (or 31), this has " + std::to_string(chars.size());
        return false;
    }
    std::vector<uint8_t> bytes;
    uint32_t buffer = 0;
    int bits = 0;
    for (char c : chars) {
        buffer = (buffer << 5) | static_cast<uint32_t>(value_of(c));
        bits += 5;
        if (bits >= 8) {
            bytes.push_back(static_cast<uint8_t>(buffer >> (bits - 8)));
            bits -= 8;
        }
    }
    // The spare bits at the end are zero in every code made.
    if (bytes.size() != size || (buffer & ((1u << bits) - 1)) != 0 ||
        check_byte(bytes.data(), size - 1) != bytes[size - 1]) {
        *why = "the code does not check out (a character mistyped?)";
        return false;
    }
    const uint16_t port = static_cast<uint16_t>((bytes[size - 3] << 8) | bytes[size - 2]);
    std::memset(addr, 0, sizeof(*addr));
    if (size == kIpv4Bytes) {
        auto& v4 = reinterpret_cast<sockaddr_in&>(*addr);
        v4.sin_family = AF_INET;
        std::memcpy(&v4.sin_addr, bytes.data(), 4);
        v4.sin_port = htons(port);
        *len = sizeof(sockaddr_in);
    } else {
        auto& v6 = reinterpret_cast<sockaddr_in6&>(*addr);
        v6.sin6_family = AF_INET6;
        std::memcpy(v6.sin6_addr.s6_addr, bytes.data(), 16);
        v6.sin6_port = htons(port);
        *len = sizeof(sockaddr_in6);
    }
#if defined(__APPLE__)
    addr->ss_len = static_cast<uint8_t>(*len);
#endif
    if (port == 0 || !host_address(*addr)) {
        *why = "the code is not a host's address";
        return false;
    }
    // A code is handed round, so it names another machine: not this one (an address typed in can).
    const bool loopback = size == kIpv4Bytes
                              ? (bytes[0] == 127)
                              : IN6_IS_ADDR_LOOPBACK(&reinterpret_cast<const sockaddr_in6&>(*addr).sin6_addr) ||
                                    (IN6_IS_ADDR_V4MAPPED(&reinterpret_cast<const sockaddr_in6&>(*addr).sin6_addr) &&
                                     bytes[12] == 127);
    if (loopback) {
        *why = "the code names this machine itself, not a host";
        return false;
    }
    return true;
}

bool read_join_target(const std::string& text_in, uint16_t default_port, sockaddr_storage* addr, socklen_t* len,
                      std::string* why) {
    std::string text = text_in;
    text.erase(0, text.find_first_not_of(" \t"));
    text.erase(text.find_last_not_of(" \t") + 1);
    if (text.empty()) {
        *why = "type the host's address or join code";
        return false;
    }
    const bool address_like = text.find_first_of(".:[") != std::string::npos;
    const std::string chars = code_characters(text);
    if (!address_like && (chars.size() == chars_for(kIpv4Bytes) || chars.size() == chars_for(kIpv6Bytes) ||
                          text.find('-') != std::string::npos)) {
        return read_join_code(text, addr, len, why);
    }
    std::string host = text;
    std::string port = std::to_string(default_port);
    if (host[0] == '[') {
        const size_t close = host.find(']');
        if (close == std::string::npos) {
            *why = "an IPv6 address in [ ] has no closing ]";
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
    const int port_number = std::atoi(port.c_str());
    if (port_number <= 0 || port_number > 65535 || port.find_first_not_of("0123456789") != std::string::npos) {
        *why = "the port after the : is not a number from 1 to 65535";
        return false;
    }
    addrinfo hints{};
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_family = AF_UNSPEC;
    addrinfo* found = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &found) != 0 || found == nullptr) {
        *why = "no such address or host name: " + host;
        return false;
    }
    std::memcpy(addr, found->ai_addr, found->ai_addrlen);
    *len = static_cast<socklen_t>(found->ai_addrlen);
    freeaddrinfo(found);
    if (!host_address(*addr)) {
        *why = host + " is not a host's address (a broadcast, multicast or empty one)";
        return false;
    }
    return true;
}

std::string address_with_port(const sockaddr_storage& addr, socklen_t /*len*/) {
    char text[INET6_ADDRSTRLEN] = "?";
    if (addr.ss_family == AF_INET) {
        const auto& v4 = reinterpret_cast<const sockaddr_in&>(addr);
        inet_ntop(AF_INET, &v4.sin_addr, text, sizeof(text));
        return std::string(text) + ":" + std::to_string(ntohs(v4.sin_port));
    }
    const auto& v6 = reinterpret_cast<const sockaddr_in6&>(addr);
    if (IN6_IS_ADDR_V4MAPPED(&v6.sin6_addr)) {
        inet_ntop(AF_INET, v6.sin6_addr.s6_addr + 12, text, sizeof(text));
        return std::string(text) + ":" + std::to_string(ntohs(v6.sin6_port));
    }
    inet_ntop(AF_INET6, &v6.sin6_addr, text, sizeof(text));
    return "[" + std::string(text) + "]:" + std::to_string(ntohs(v6.sin6_port));
}

bool lan_address(const sockaddr_storage& addr) {
    uint32_t v4 = 0;
    if (addr.ss_family == AF_INET) {
        v4 = ntohl(reinterpret_cast<const sockaddr_in&>(addr).sin_addr.s_addr);
    } else if (addr.ss_family == AF_INET6) {
        const in6_addr& a = reinterpret_cast<const sockaddr_in6&>(addr).sin6_addr;
        if (!IN6_IS_ADDR_V4MAPPED(&a)) {
            return IN6_IS_ADDR_LOOPBACK(&a) || IN6_IS_ADDR_LINKLOCAL(&a) || (a.s6_addr[0] & 0xFE) == 0xFC;
        }
        std::memcpy(&v4, a.s6_addr + 12, 4);
        v4 = ntohl(v4);
    } else {
        return false;
    }
    const AddressKind kind = ipv4_kind(v4);
    return kind == AddressKind::Private || kind == AddressKind::Loopback || kind == AddressKind::LinkLocal;
}

namespace {
std::atomic<bool> g_internet_allowed{false};
}

void set_internet_allowed(bool allowed) { g_internet_allowed.store(allowed); }

bool internet_allowed() { return g_internet_allowed.load(); }

bool sender_allowed(const sockaddr_storage& from) {
    static const int test_port = [] {
        const char* value = std::getenv("CONKER_NET_TEST_INTERNET_PORT");
        return value != nullptr ? std::atoi(value) : 0;
    }();
    if (internet_allowed()) {
        return true;
    }
    if (test_port > 0) {
        const uint16_t port = from.ss_family == AF_INET ? ntohs(reinterpret_cast<const sockaddr_in&>(from).sin_port)
                              : from.ss_family == AF_INET6 ? ntohs(reinterpret_cast<const sockaddr_in6&>(from).sin6_port)
                                                           : 0;
        if (port == test_port) {
            return false;
        }
    }
    return lan_address(from);
}

std::string internet_off_message() {
    return "internet play is off: this build joins and hosts on the LAN only until its online security is "
           "trusted. Start the game with --net-unsafe-internet to allow it (docs/online-play.md)";
}

AddressKind ipv4_kind(uint32_t a) {
    if ((a >> 24) == 10 || (a >> 20) == (172 << 4 | 1) || (a >> 16) == (192 << 8 | 168)) {
        return AddressKind::Private;
    }
    if ((a >> 22) == ((100 << 2) | 1)) {  // 100.64.0.0/10
        return AddressKind::CarrierGradeNat;
    }
    if ((a >> 24) == 127) {
        return AddressKind::Loopback;
    }
    if ((a >> 16) == (169 << 8 | 254)) {
        return AddressKind::LinkLocal;
    }
    if (a == 0 || (a >> 28) >= 14) {  // 0.0.0.0, multicast and reserved
        return AddressKind::Other;
    }
    return AddressKind::Public;
}

LocalAddresses local_addresses() {
    LocalAddresses out;
    // One address of an interface that is up and not loopback, with its IPv4 broadcast address if any.
    const auto add = [&out](const sockaddr* address, const uint32_t* broadcast) {
        sockaddr_storage addr{};
        if (address->sa_family == AF_INET) {
            std::memcpy(&addr, address, sizeof(sockaddr_in));
            const uint32_t a = ntohl(reinterpret_cast<sockaddr_in&>(addr).sin_addr.s_addr);
            const AddressKind kind = ipv4_kind(a);
            if (kind == AddressKind::Loopback || kind == AddressKind::LinkLocal || kind == AddressKind::Other) {
                return;
            }
            out.ipv4.push_back(addr);
            if (broadcast != nullptr &&
                std::find(out.ipv4_broadcasts.begin(), out.ipv4_broadcasts.end(), *broadcast) == out.ipv4_broadcasts.end()) {
                out.ipv4_broadcasts.push_back(*broadcast);
            }
        } else if (address->sa_family == AF_INET6) {
            std::memcpy(&addr, address, sizeof(sockaddr_in6));
            const in6_addr& a = reinterpret_cast<sockaddr_in6&>(addr).sin6_addr;
            if ((a.s6_addr[0] & 0xE0) != 0x20) {  // global unicast only
                return;
            }
            bool seen = false;
            for (const sockaddr_storage& other : out.ipv6) {
                seen = seen || std::memcmp(&reinterpret_cast<const sockaddr_in6&>(other).sin6_addr, &a, sizeof(a)) == 0;
            }
            if (!seen) {
                out.ipv6.push_back(addr);
            }
        }
    };
#if defined(_WIN32)
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buffer;
    ULONG result = ERROR_BUFFER_OVERFLOW;
    for (int tries = 0; tries < 3 && result == ERROR_BUFFER_OVERFLOW; tries++) {
        buffer.resize(size);
        result = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                      nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    }
    if (result != NO_ERROR) {
        return out;
    }
    for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); adapter != nullptr; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
            continue;
        }
        for (auto* unicast = adapter->FirstUnicastAddress; unicast != nullptr; unicast = unicast->Next) {
            const sockaddr* address = unicast->Address.lpSockaddr;
            if (address == nullptr) {
                continue;
            }
            uint32_t broadcast = 0;
            const bool has_broadcast = address->sa_family == AF_INET && unicast->OnLinkPrefixLength < 31;
            if (has_broadcast) {
                const uint32_t a = ntohl(reinterpret_cast<const sockaddr_in*>(address)->sin_addr.s_addr);
                const uint32_t host_bits = unicast->OnLinkPrefixLength == 0 ? 0xFFFFFFFFu : (0xFFFFFFFFu >> unicast->OnLinkPrefixLength);
                broadcast = a | host_bits;
            }
            add(address, has_broadcast ? &broadcast : nullptr);
        }
    }
#else
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) {
        return out;
    }
    for (ifaddrs* ifa = list; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr || (ifa->ifa_flags & IFF_UP) == 0 || (ifa->ifa_flags & IFF_LOOPBACK) != 0) {
            continue;
        }
        uint32_t broadcast = 0;
        const bool has_broadcast = ifa->ifa_addr->sa_family == AF_INET && (ifa->ifa_flags & IFF_BROADCAST) != 0 &&
                                   ifa->ifa_broadaddr != nullptr && ifa->ifa_broadaddr->sa_family == AF_INET;
        if (has_broadcast) {
            broadcast = ntohl(reinterpret_cast<const sockaddr_in*>(ifa->ifa_broadaddr)->sin_addr.s_addr);
        }
        add(ifa->ifa_addr, has_broadcast ? &broadcast : nullptr);
    }
    freeifaddrs(list);
#endif
    return out;
}

bool local_network_address(const sockaddr_storage& addr) {
    uint32_t v4 = 0;
    if (addr.ss_family == AF_INET) {
        v4 = ntohl(reinterpret_cast<const sockaddr_in&>(addr).sin_addr.s_addr);
    } else if (addr.ss_family == AF_INET6) {
        const in6_addr& a = reinterpret_cast<const sockaddr_in6&>(addr).sin6_addr;
        if (!IN6_IS_ADDR_V4MAPPED(&a)) {
            return IN6_IS_ADDR_LOOPBACK(&a) || IN6_IS_ADDR_LINKLOCAL(&a) || (a.s6_addr[0] & 0xFE) == 0xFC;
        }
        std::memcpy(&v4, a.s6_addr + 12, 4);
        v4 = ntohl(v4);
    } else {
        return false;
    }
    const AddressKind kind = ipv4_kind(v4);
    return kind == AddressKind::Private || kind == AddressKind::Loopback || kind == AddressKind::LinkLocal ||
           kind == AddressKind::CarrierGradeNat;
}

uint64_t random64() {
    static std::mutex mutex;
    static std::random_device device;
    std::lock_guard lock(mutex);
    uint64_t value = 0;
    while (value == 0) {
        value = (static_cast<uint64_t>(device()) << 32) ^ device();
    }
    return value;
}

namespace {

uint64_t rotl(uint64_t x, int b) { return (x << b) | (x >> (64 - b)); }

void sip_round(uint64_t& v0, uint64_t& v1, uint64_t& v2, uint64_t& v3) {
    v0 += v1; v1 = rotl(v1, 13); v1 ^= v0; v0 = rotl(v0, 32);
    v2 += v3; v3 = rotl(v3, 16); v3 ^= v2;
    v0 += v3; v3 = rotl(v3, 21); v3 ^= v0;
    v2 += v1; v1 = rotl(v1, 17); v1 ^= v2; v2 = rotl(v2, 32);
}

}  // namespace

uint64_t keyed_hash(const uint64_t key[2], const void* data, size_t size) {
    const uint8_t* in = static_cast<const uint8_t*>(data);
    uint64_t v0 = 0x736F6D6570736575ull ^ key[0];
    uint64_t v1 = 0x646F72616E646F6Dull ^ key[1];
    uint64_t v2 = 0x6C7967656E657261ull ^ key[0];
    uint64_t v3 = 0x7465646279746573ull ^ key[1];
    const size_t whole = size - size % 8;
    for (size_t at = 0; at < whole; at += 8) {
        uint64_t m = 0;
        for (int i = 0; i < 8; i++) {
            m |= static_cast<uint64_t>(in[at + i]) << (8 * i);
        }
        v3 ^= m;
        sip_round(v0, v1, v2, v3);
        sip_round(v0, v1, v2, v3);
        v0 ^= m;
    }
    uint64_t last = static_cast<uint64_t>(size) << 56;
    for (size_t i = 0; i < size % 8; i++) {
        last |= static_cast<uint64_t>(in[whole + i]) << (8 * i);
    }
    v3 ^= last;
    sip_round(v0, v1, v2, v3);
    sip_round(v0, v1, v2, v3);
    v0 ^= last;
    v2 ^= 0xFF;
    for (int i = 0; i < 4; i++) {
        sip_round(v0, v1, v2, v3);
    }
    return v0 ^ v1 ^ v2 ^ v3;
}

namespace {

uint64_t cookie_for(const sockaddr_storage& addr, uint32_t purpose, int64_t epoch) {
    static const uint64_t key[2] = {random64(), random64()};
    const std::string text = address_with_port(addr, 0) + "/" + std::to_string(purpose) + "/" + std::to_string(epoch);
    return keyed_hash(key, text.data(), text.size()) | 1;
}

int64_t cookie_epoch() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count() / 30;
}

}  // namespace

uint64_t address_cookie(const sockaddr_storage& addr, uint32_t purpose) { return cookie_for(addr, purpose, cookie_epoch()); }

bool address_cookie_ok(const sockaddr_storage& addr, uint32_t purpose, uint64_t cookie) {
    const int64_t epoch = cookie_epoch();
    // Both checked, whichever matches.
    const bool now = same_secret(cookie, cookie_for(addr, purpose, epoch));
    const bool before = same_secret(cookie, cookie_for(addr, purpose, epoch - 1));
    return cookie != 0 && (now | before);
}

std::string printable(const std::string& text, size_t max, bool newlines) {
    std::string out;
    for (const char c : text) {
        if (out.size() >= max) {
            break;
        }
        if ((c >= 32 && c < 127) || (newlines && c == '\n')) {
            out += c;
        }
    }
    return out;
}

}  // namespace conker::net
