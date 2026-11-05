// Online play's traffic, counted; see net_stats.hpp.
#include "net/net_stats.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

namespace conker::net::stats {
namespace {

// net_play's packet types (its enum Type), by number; 0: a datagram not sealed (the key OFFER).
constexpr const char* kTypeNames[] = {"OFFER", "HELLO",    "REJECT",    "CHUNK",   "READY",  "START",  "STARTED",
                                      "INPUT", "MERGED",   "SNAPWANT",  "SNAPCHUNK", "SNAPACK", "COOKIE", "LEAVE"};
constexpr size_t kTypes = sizeof(kTypeNames) / sizeof(kTypeNames[0]);
constexpr const char* kPartNames[kPartCount] = {"ip+udp", "seal", "header", "times", "frames", "records", "hashes", "drops"};

struct Count {
    uint64_t packets = 0;
    uint64_t bytes = 0;
};

struct Updates {  // the INPUT or MERGED packets sent
    uint64_t parts[kPartCount] = {};
    uint64_t records = 0;
    uint64_t fresh = 0;
};

std::mutex g_mutex;
int g_interval_ms = -1;  // -1: not read yet; 0: off
bool g_host = false;
bool g_running = false;
int64_t g_start_ms = 0;
int64_t g_next_ms = 0;
uint64_t g_frames = 0;
std::array<Count, kTypes> g_sent{};
std::array<Count, kTypes> g_received{};
Updates g_updates[2];  // [0] INPUT, [1] MERGED
// At the last report, for the figures since it (a match after the menus, say).
int64_t g_last_ms = 0;
uint64_t g_last_frames = 0;
Count g_last_sent;
Count g_last_received;

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

int interval_ms() {
    if (g_interval_ms < 0) {
        const char* value = std::getenv("CONKER_NET_STATS");
        const int seconds = value != nullptr ? std::atoi(value) : 0;
        g_interval_ms = seconds <= 0 ? 0 : (seconds == 1 ? 10 : seconds) * 1000;
    }
    return g_interval_ms;
}

Count total(const std::array<Count, kTypes>& counts) {
    Count sum;
    for (const Count& c : counts) {
        sum.packets += c.packets;
        sum.bytes += c.bytes;
    }
    return sum;
}

// Under g_mutex.
void print_report(int64_t now) {
    const double seconds = std::max<int64_t>(now - g_start_ms, 1) / 1000.0;
    const double frames = static_cast<double>(std::max<uint64_t>(g_frames, 1));
    const Count sent = total(g_sent);
    const Count received = total(g_received);
    std::string text;
    char line[256];
    std::snprintf(line, sizeof(line),
                  "[conker] net stats (%s): %.1f s, %llu frames (%.1f a second)\n"
                  "[conker] net stats   sent     %8.2f KB/s %7.1f packets/s %6.1f B/packet %7.1f B/frame\n"
                  "[conker] net stats   received %8.2f KB/s %7.1f packets/s %6.1f B/packet %7.1f B/frame\n",
                  g_host ? "host" : "guest", seconds, static_cast<unsigned long long>(g_frames), g_frames / seconds,
                  sent.bytes / seconds / 1000.0, sent.packets / seconds,
                  sent.packets != 0 ? static_cast<double>(sent.bytes) / sent.packets : 0.0, sent.bytes / frames,
                  received.bytes / seconds / 1000.0, received.packets / seconds,
                  received.packets != 0 ? static_cast<double>(received.bytes) / received.packets : 0.0,
                  received.bytes / frames);
    text += line;
    const double frames_since = static_cast<double>(std::max<uint64_t>(g_frames - g_last_frames, 1));
    const double seconds_since = std::max<int64_t>(now - g_last_ms, 1) / 1000.0;
    std::snprintf(line, sizeof(line),
                  "[conker] net stats   since the last report: %.1f s, %llu frames, sent %.2f KB/s %.1f B/frame, "
                  "received %.2f KB/s %.1f B/frame\n",
                  seconds_since, static_cast<unsigned long long>(g_frames - g_last_frames),
                  (sent.bytes - g_last_sent.bytes) / seconds_since / 1000.0, (sent.bytes - g_last_sent.bytes) / frames_since,
                  (received.bytes - g_last_received.bytes) / seconds_since / 1000.0,
                  (received.bytes - g_last_received.bytes) / frames_since);
    text += line;
    g_last_ms = now;
    g_last_frames = g_frames;
    g_last_sent = sent;
    g_last_received = received;
    for (int direction = 0; direction < 2; direction++) {
        const std::array<Count, kTypes>& counts = direction == 0 ? g_sent : g_received;
        for (size_t type = 0; type < kTypes; type++) {
            const Count& c = counts[type];
            if (c.packets == 0) {
                continue;
            }
            std::snprintf(line, sizeof(line),
                          "[conker] net stats     %-8s %-9s %8llu packets %10llu B %6.1f B/packet %7.1f B/frame\n",
                          direction == 0 ? "sent" : "received", kTypeNames[type],
                          static_cast<unsigned long long>(c.packets), static_cast<unsigned long long>(c.bytes),
                          static_cast<double>(c.bytes) / c.packets, c.bytes / frames);
            text += line;
        }
    }
    for (int merged = 0; merged < 2; merged++) {
        const Updates& u = g_updates[merged];
        const Count& c = g_sent[merged ? 8 : 7];
        if (c.packets == 0) {
            continue;
        }
        std::snprintf(line, sizeof(line), "[conker] net stats   %s sent, per packet:", merged ? "MERGED" : "INPUT");
        text += line;
        for (int part = 0; part < kPartCount; part++) {
            std::snprintf(line, sizeof(line), " %s %.1f", kPartNames[part], static_cast<double>(u.parts[part]) / c.packets);
            text += line;
        }
        std::snprintf(line, sizeof(line), " (B); %.2f records, %.2f new\n", static_cast<double>(u.records) / c.packets,
                      static_cast<double>(u.fresh) / c.packets);
        text += line;
    }
    std::fputs(text.c_str(), stdout);
    std::fflush(stdout);
}

}  // namespace

bool enabled() {
    std::lock_guard lock(g_mutex);
    return interval_ms() != 0;
}

void begin_session(bool host) {
    std::lock_guard lock(g_mutex);
    if (interval_ms() == 0) {
        return;
    }
    g_host = host;
    g_running = true;
    g_start_ms = now_ms();
    g_next_ms = g_start_ms + g_interval_ms;
    g_last_ms = g_start_ms;
    g_last_frames = 0;
    g_last_sent = {};
    g_last_received = {};
    g_frames = 0;
    g_sent = {};
    g_received = {};
    g_updates[0] = {};
    g_updates[1] = {};
}

size_t wire_bytes(size_t size, bool ipv6) { return size + (ipv6 ? 48 : 28); }

void sent(uint8_t type, size_t wire) {
    std::lock_guard lock(g_mutex);
    if (!g_running) {
        return;
    }
    Count& c = g_sent[type < kTypes ? type : 0];
    c.packets++;
    c.bytes += wire;
}

void received(uint8_t type, size_t wire) {
    std::lock_guard lock(g_mutex);
    if (!g_running) {
        return;
    }
    Count& c = g_received[type < kTypes ? type : 0];
    c.packets++;
    c.bytes += wire;
}

void part(bool merged, Part part, size_t bytes) {
    std::lock_guard lock(g_mutex);
    if (g_running && part < kPartCount) {
        g_updates[merged ? 1 : 0].parts[part] += bytes;
    }
}

void records(bool merged, uint32_t count, uint32_t fresh) {
    std::lock_guard lock(g_mutex);
    if (g_running) {
        g_updates[merged ? 1 : 0].records += count;
        g_updates[merged ? 1 : 0].fresh += fresh;
    }
}

void frame_played() {
    std::lock_guard lock(g_mutex);
    if (g_running) {
        g_frames++;
    }
}

void tick() {
    std::lock_guard lock(g_mutex);
    const int64_t at = now_ms();
    if (!g_running || at < g_next_ms) {
        return;
    }
    g_next_ms = at + g_interval_ms;
    print_report(at);
}

void report() {
    std::lock_guard lock(g_mutex);
    if (!g_running) {
        return;
    }
    print_report(now_ms());
    g_running = false;
}

}  // namespace conker::net::stats
