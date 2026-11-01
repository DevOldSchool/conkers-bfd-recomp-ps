// Opening the host's port on its router; see net_portmap.hpp.
#include "net/net_portmap.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#if !defined(_WIN32)
#include <sys/select.h>
#endif

#include "net/net_codes.hpp"

#if CONKER_PORT_MAPPING
#include <miniupnpc.h>
#include <natpmp.h>
#include <upnpcommands.h>
#include <upnperrors.h>
// libnatpmp's (getgateway.h, not copied out with its other headers).
extern "C" int getdefaultgateway(in_addr_t* addr);
#endif

namespace conker::net::portmap {

namespace {

using State = Status::State;

// A short lease, renewed often: a mapping a crashed or killed game leaves behind closes by itself soon
// (the exit removes it otherwise). Permanent leases are never asked for.
constexpr int kLeaseSeconds = 600;
constexpr auto kRenewEvery = std::chrono::minutes(4);
constexpr const char* kDescription = "Conker's Bad Fur Day Recompiled online";

std::mutex g_mutex;
std::condition_variable g_wake;
Status g_status;
bool g_started = false;
uint64_t g_generation = 0;     // open() and close() each move it on: a worker of an older one stops
bool g_worker_running = false; // one worker at a time talks to the router
uint16_t g_port = 0;

void set_status(State state, const std::string& method, const std::string& external, const std::string& message) {
    std::lock_guard lock(g_mutex);
    g_status = {state, method, external, message};
}

std::string port_text() { return "UDP port " + std::to_string(g_port); }

// The sentence for an opened port, from the router's outside address.
std::string opened_message(const std::string& method, const std::string& external) {
    std::string text = port_text() + " is open on the router (" + method + ").";
    in_addr a{};
    if (external.empty() || inet_pton(AF_INET, external.c_str(), &a) != 1) {
        return text + " The router did not say its outside address.";
    }
    switch (ipv4_kind(ntohl(a.s_addr))) {
        case AddressKind::Public:
            return text + " Outside address: " + external + ".";
        case AddressKind::CarrierGradeNat:
            return text + " But the router's outside address, " + external +
                   ", is one the internet provider shares between customers (carrier-grade NAT), so guests on "
                   "other networks still cannot reach this machine over IPv4: use IPv6, or an overlay network "
                   "such as Tailscale or ZeroTier.";
        default:
            return text + " But the router's outside address, " + external +
                   ", is a private one: another router sits in front of it (double NAT) and needs the port "
                   "forwarded too, or use an overlay network such as Tailscale or ZeroTier.";
    }
}

std::string failed_message(const std::string& why) {
    return "Could not open " + port_text() + " on the router (" + why +
           "). Players on this network can join anyway. For players on other networks, forward " + port_text() +
           " to this machine in the router's settings, or use an overlay network such as Tailscale or ZeroTier "
           "(see docs/online-play.md).";
}

#if CONKER_PORT_MAPPING

// What was mapped, to renew and remove it.
enum class Method { None, Upnp, Natpmp };
Method g_method = Method::None;
UPNPUrls g_urls{};
IGDdatas g_data{};
char g_lan[64] = {};
bool g_natpmp_forced = false;
in_addr_t g_natpmp_gateway = 0;

// Whether a discovered UPnP device's URLs are on the default gateway. Any device on the network can
// answer discovery and name any URL; only the router itself is asked to map the port (and believed
// about the outside address).
bool on_gateway(const UPNPUrls& urls) {
    in_addr_t gateway = 0;
    if (getdefaultgateway(&gateway) != 0 || gateway == 0) {
        return false;
    }
    for (const char* url : {urls.rootdescURL, urls.controlURL}) {
        if (url == nullptr || std::strncmp(url, "http://", 7) != 0) {
            return false;
        }
        const char* host = url + 7;
        const size_t length = std::strcspn(host, ":/");
        const std::string text(host, length);
        in_addr a{};
        if (inet_pton(AF_INET, text.c_str(), &a) != 1 || a.s_addr != gateway) {
            return false;
        }
    }
    return true;
}

// Adds (or renews) the UPnP mapping; the UPnP error code, 0 for none.
int upnp_add() {
    const std::string port = std::to_string(g_port);
    const std::string lease = std::to_string(kLeaseSeconds);
    // A router that takes only permanent mappings (725, OnlyPermanentLeasesSupported) is not given one:
    // a game that crashed would leave the port open for good.
    return UPNP_AddPortMapping(g_urls.controlURL, g_data.first.servicetype, port.c_str(), port.c_str(), g_lan,
                               kDescription, "UDP", nullptr, lease.c_str());
}

bool try_upnp(std::string* why, std::string* external) {
    const char* forced = std::getenv("CONKER_NET_UPNP_URL");
    char wan[64] = {};
    int found = 0;
    if (forced != nullptr && forced[0] != '\0') {
        found = UPNP_GetIGDFromUrl(forced, &g_urls, &g_data, g_lan, sizeof(g_lan));
    } else {
        int error = 0;
        UPNPDev* devices = upnpDiscover(2000, nullptr, nullptr, UPNP_LOCAL_PORT_ANY, 0, 2, &error);
        if (devices == nullptr) {
            *why = "no UPnP router answered";
            return false;
        }
        found = UPNP_GetValidIGD(devices, &g_urls, &g_data, g_lan, sizeof(g_lan), wan, sizeof(wan));
        freeUPNPDevlist(devices);
        if (found != 1 && found != 2) {  // a connected router, or one with a private outside address
            if (found != 0) {
                FreeUPNPUrls(&g_urls);
            }
            *why = "no connected UPnP router";
            return false;
        }
        if (!on_gateway(g_urls)) {
            FreeUPNPUrls(&g_urls);
            *why = "the UPnP device that answered is not the router (the default gateway)";
            return false;
        }
    }
    if (found == 0) {
        *why = "no UPnP router at CONKER_NET_UPNP_URL";
        return false;
    }
    const int result = upnp_add();
    if (result != UPNPCOMMAND_SUCCESS) {
        *why = result == 718   ? "the router already forwards it to another machine"
               : result == 725 ? "the router takes only permanent mappings, which this game does not make"
                               : std::string("UPnP said: ") + strupnperror(result);
        FreeUPNPUrls(&g_urls);
        return false;
    }
    char ext[64] = {};
    if (UPNP_GetExternalIPAddress(g_urls.controlURL, g_data.first.servicetype, ext) == UPNPCOMMAND_SUCCESS) {
        *external = ext;
    } else {
        *external = wan;
    }
    g_method = Method::Upnp;
    return true;
}

void upnp_remove() {
    const std::string port = std::to_string(g_port);
    UPNP_DeletePortMapping(g_urls.controlURL, g_data.first.servicetype, port.c_str(), "UDP", nullptr);
    FreeUPNPUrls(&g_urls);
}

// One NAT-PMP request already sent: its answer, waiting no longer than `ms`.
int natpmp_answer(natpmp_t* n, natpmpresp_t* response, int ms) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    int result = NATPMP_TRYAGAIN;
    while (result == NATPMP_TRYAGAIN && std::chrono::steady_clock::now() < until) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(n->s, &fds);
        timeval timeout{};
        getnatpmprequesttimeout(n, &timeout);
        const auto left = std::chrono::duration_cast<std::chrono::microseconds>(until - std::chrono::steady_clock::now());
        if (timeout.tv_sec * 1000000LL + timeout.tv_usec > left.count()) {
            timeout.tv_sec = static_cast<decltype(timeout.tv_sec)>(left.count() / 1000000);
            timeout.tv_usec = static_cast<decltype(timeout.tv_usec)>(left.count() % 1000000);
        }
        select(static_cast<int>(n->s) + 1, &fds, nullptr, nullptr, &timeout);
        result = readnatpmpresponseorretry(n, response);
    }
    return result;
}

// Maps (lifetime > 0) or removes (0) the port; the libnatpmp result.
int natpmp_map(uint32_t lifetime, std::string* external) {
    natpmp_t n{};
    if (initnatpmp(&n, g_natpmp_forced ? 1 : 0, g_natpmp_gateway) != 0) {
        return NATPMP_ERR_CANNOTGETGATEWAY;
    }
    natpmpresp_t response{};
    int result = 0;
    if (external != nullptr) {
        sendpublicaddressrequest(&n);
        result = natpmp_answer(&n, &response, 2500);
        if (result != 0) {
            closenatpmp(&n);
            return result;
        }
        *external = inet_ntoa(response.pnu.publicaddress.addr);
    }
    sendnewportmappingrequest(&n, NATPMP_PROTOCOL_UDP, g_port, g_port, lifetime);
    result = natpmp_answer(&n, &response, 2500);
    if (result == 0 && lifetime > 0 && response.pnu.newportmapping.mappedpublicport != g_port) {
        // The router gave another outside port; this game asks guests for the same number.
        sendnewportmappingrequest(&n, NATPMP_PROTOCOL_UDP, g_port, response.pnu.newportmapping.mappedpublicport, 0);
        natpmp_answer(&n, &response, 1000);
        result = NATPMP_ERR_OUTOFRESOURCES;
    }
    closenatpmp(&n);
    return result;
}

bool try_natpmp(std::string* why, std::string* external) {
    const char* forced = std::getenv("CONKER_NET_NATPMP_GATEWAY");
    if (forced != nullptr && forced[0] != '\0') {
        g_natpmp_forced = true;
        g_natpmp_gateway = inet_addr(forced);
    }
    const int result = natpmp_map(kLeaseSeconds, external);
    if (result != 0) {
        *why = result == NATPMP_TRYAGAIN || result == NATPMP_ERR_NOGATEWAYSUPPORT
                   ? "no NAT-PMP answer"
                   : std::string("NAT-PMP said: ") + strnatpmperr(result);
        return false;
    }
    g_method = Method::Natpmp;
    return true;
}

void remove_mapping() {
    if (g_method == Method::Upnp) {
        upnp_remove();
    } else if (g_method == Method::Natpmp) {
        natpmp_map(0, nullptr);
    }
    if (g_method != Method::None) {
        std::printf("[conker] net: UDP port %u closed on the router again\n", g_port);
    }
    g_method = Method::None;
}

void worker(uint64_t generation) {
    {
        std::unique_lock lock(g_mutex);
        g_wake.wait(lock, [] { return !g_worker_running; });  // an older one still removing its mapping
        if (generation != g_generation) {
            return;
        }
        g_worker_running = true;
    }
    const char* upnp_url = std::getenv("CONKER_NET_UPNP_URL");
    const char* natpmp_gateway = std::getenv("CONKER_NET_NATPMP_GATEWAY");
    const bool forced = upnp_url != nullptr || natpmp_gateway != nullptr;
    std::string upnp_why = "not tried";
    std::string natpmp_why = "not tried";
    std::string external;
    bool opened = false;
    std::string method;
    if (!forced || upnp_url != nullptr) {
        opened = try_upnp(&upnp_why, &external);
        method = "UPnP";
    }
    if (!opened && (!forced || natpmp_gateway != nullptr)) {
        opened = try_natpmp(&natpmp_why, &external);
        method = "NAT-PMP";
    }
    std::unique_lock lock(g_mutex);
    if (!opened) {
        if (generation == g_generation) {
            g_status = {State::Failed, "", "", failed_message(upnp_why + "; " + natpmp_why)};
        }
        std::printf("[conker] net: could not open UDP port %u on the router: %s; %s\n", g_port, upnp_why.c_str(),
                    natpmp_why.c_str());
        g_worker_running = false;
        g_wake.notify_all();
        return;
    }
    if (generation == g_generation) {
        g_status = {State::Opened, method, external, opened_message(method, external)};
    }
    std::printf("[conker] net: UDP port %u opened on the router with %s (outside address %s)\n", g_port,
                method.c_str(), external.empty() ? "not given" : external.c_str());
    while (generation == g_generation) {
        if (g_wake.wait_for(lock, kRenewEvery, [generation] { return generation != g_generation; })) {
            break;
        }
        std::string ignored;
        const bool renewed = g_method == Method::Upnp ? upnp_add() == UPNPCOMMAND_SUCCESS
                                                      : natpmp_map(kLeaseSeconds, nullptr) == 0;
        std::printf("[conker] net: renewing UDP port %u on the router: %s\n", g_port, renewed ? "done" : "failed");
    }
    // close() came: the mapping goes (close() leaves it to this worker, which owns it).
    remove_mapping();
    g_worker_running = false;
    g_wake.notify_all();
}

#else

void worker(uint64_t) {
    set_status(State::Failed, "", "", failed_message("this build has no UPnP or NAT-PMP"));
}

void remove_mapping() {}

#endif

}  // namespace

void open(uint16_t port) {
    {
        std::lock_guard lock(g_mutex);
        if (g_started) {
            return;
        }
        g_started = true;
        g_port = port;
        if (!internet_allowed()) {
            g_status = {State::Off, "", "",
                        "Nothing is opened on the router: internet play is off."};
            return;
        }
        const char* off = std::getenv("CONKER_NET_PORTMAP");
        if (off != nullptr && off[0] == '0') {
            g_status = {State::Off, "", "", "Opening the port on the router is off (CONKER_NET_PORTMAP=0)."};
            return;
        }
        g_status = {State::Working, "", "", "Asking the router to open " + port_text() + "..."};
    }
    static bool at_exit = (std::atexit(close), true);
    (void)at_exit;
    uint64_t generation;
    {
        std::lock_guard lock(g_mutex);
        generation = ++g_generation;
    }
    std::thread(worker, generation).detach();
}

Status status() {
    std::lock_guard lock(g_mutex);
    return g_status;
}

void close() {
    std::unique_lock lock(g_mutex);
    if (!g_started) {
        return;
    }
    // The worker removes its mapping and stops; HOST again later opens the port again.
    g_started = false;
    g_generation++;
    g_status = {State::Off, "", "", ""};
    g_wake.notify_all();
    // At exit (or LEAVE) wait for the removal, a few seconds at most, so that it is done before the
    // process goes.
    g_wake.wait_for(lock, std::chrono::seconds(6), [] { return !g_worker_running; });
}

}  // namespace conker::net::portmap
