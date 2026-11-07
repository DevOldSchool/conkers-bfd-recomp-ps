// A snapshot of the running game for rejoining an online session: see snapshot.hpp.
#include "runtime/snapshot.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <functional>
#include <mutex>

#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"

// librecomp's save buffer (pi.cpp), through RDRAM.
void save_read(uint8_t* rdram, int32_t rdram_address, uint32_t offset, uint32_t count);
void save_write(uint8_t* rdram, int32_t rdram_address, uint32_t offset, uint32_t count);

namespace conker::snapshot {
namespace {

constexpr uint32_t kMagic = 0x50414E53;     // "SNAP"
constexpr uint32_t kVersion = 2;  // 2: a thread's place written field by field (no padding)
constexpr uint32_t kGameMemory = 0x400000;  // the game's 4 MiB; above it is the runtime's scratch
constexpr uint32_t kSaveScratch = 0x807F8000;  // 2 KiB of scratch RDRAM to pass the save through
constexpr uint32_t kSaveSize = 0x800;          // 16 kbit EEPROM
constexpr uint32_t kOsTimerSize = 0x20;        // libultra's OSTimer (ultramodern keeps its layout to itself)

struct Field {
    std::string name;
    std::function<std::vector<uint8_t>()> save;
    std::function<bool(const std::vector<uint8_t>&)> load;
    std::function<bool(const std::vector<uint8_t>&)> check;  // before anything is put in place (or none)
};
std::vector<Field>& registered_fields() {
    static std::vector<Field> list;
    return list;
}
// In name order: modules register their fields from static initialisers, whose order between files
// is the linker's, and a snapshot goes between machines built by different ones (Macs, Linux, Windows).
const std::vector<Field>& fields() {
    static std::vector<Field> sorted;
    if (sorted.size() != registered_fields().size()) {
        sorted = registered_fields();
        std::stable_sort(sorted.begin(), sorted.end(), [](const Field& a, const Field& b) { return a.name < b.name; });
    }
    return sorted;
}
std::vector<std::function<const char*()>>& checks() {
    static std::vector<std::function<const char*()>> list;
    return list;
}
const char* failed_check() {
    for (const auto& check : checks()) {
        if (const char* why = check()) {
            return why;
        }
    }
    return nullptr;
}

struct Thread {
    PTR(OSThread) thread;
    recomp_context* ctx;
};
std::atomic<bool> g_discard_framebuffers{false};
std::mutex g_threads_mutex;
std::vector<Thread> g_threads;  // every game thread started, in start order

// What a thread's registers hold that a snapshot carries (not f_odd, a pointer into the context).
struct Registers {
    gpr r[32];
    uint64_t f[32];
    uint64_t hi, lo;
    uint32_t status_reg;
    uint8_t mips3_float_mode;
};

Registers registers_of(const recomp_context* ctx) {
    Registers regs{};
    const gpr* r = &ctx->r0;
    const fpr* f = &ctx->f0;
    for (int i = 0; i < 32; i++) {
        regs.r[i] = r[i];
        regs.f[i] = f[i].u64;
    }
    regs.hi = ctx->hi;
    regs.lo = ctx->lo;
    regs.status_reg = ctx->status_reg;
    regs.mips3_float_mode = ctx->mips3_float_mode;
    return regs;
}

void set_registers(recomp_context* ctx, const Registers& regs) {
    gpr* r = &ctx->r0;
    fpr* f = &ctx->f0;
    for (int i = 0; i < 32; i++) {
        r[i] = regs.r[i];
        f[i].u64 = regs.f[i];
    }
    ctx->hi = regs.hi;
    ctx->lo = regs.lo;
    ctx->status_reg = regs.status_reg;
    ctx->mips3_float_mode = regs.mips3_float_mode;
}

// Where a thread is: running (the one taking or loading the snapshot), or waiting at a call (its
// return address) on a queue.
struct Place {
    uint32_t thread;
    uint16_t state;
    uint32_t queue;
    uint32_t ra;
};

Place place_of(uint8_t* rdram, const Thread& t) {
    const OSThread* thread = TO_PTR(OSThread, t.thread);
    return {static_cast<uint32_t>(t.thread), thread->state, static_cast<uint32_t>(thread->queue),
            static_cast<uint32_t>(t.ctx->r31)};
}

struct Writer {
    std::vector<uint8_t> data;
    void bytes(const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        data.insert(data.end(), b, b + n);
    }
    template <typename T> void put(const T& v) { bytes(&v, sizeof(v)); }
    void blob(const std::vector<uint8_t>& v) {
        put(static_cast<uint32_t>(v.size()));
        bytes(v.data(), v.size());
    }
};

struct Reader {
    const std::vector<uint8_t>& data;
    size_t at = 0;
    bool ok = true;
    bool bytes(void* p, size_t n) {
        if (!ok || at + n > data.size()) {
            ok = false;
            return false;
        }
        std::memcpy(p, data.data() + at, n);
        at += n;
        return true;
    }
    template <typename T> T get() {
        T v{};
        bytes(&v, sizeof(v));
        return v;
    }
    std::vector<uint8_t> blob() {
        const uint32_t n = get<uint32_t>();
        if (!ok || at + n > data.size()) {
            ok = false;
            return {};
        }
        std::vector<uint8_t> v(data.begin() + static_cast<std::ptrdiff_t>(at), data.begin() + static_cast<std::ptrdiff_t>(at + n));
        at += n;
        return v;
    }
};

std::string hex(uint32_t v) {
    char text[16];
    std::snprintf(text, sizeof(text), "%08X", v);
    return text;
}

// The snapshot's game memory, read as the runtime will read it once it is in place.
struct Image {
    const uint8_t* data;  // kGameMemory bytes, RDRAM's layout
    // A word at a KSEG0 address in the game's memory, or false.
    bool word(uint32_t address, uint32_t* value) const {
        if (address < 0x80000000u || address > 0x80000000u + kGameMemory - 4 || address % 4 != 0) {
            return false;
        }
        std::memcpy(value, data + (address - 0x80000000u), 4);
        return true;
    }
};

// The runtime's own structures in the snapshot's game memory, checked before it is put in place: the
// runtime takes native pointers and buffer indices from them, and the snapshot is another
// machine's. Every list of threads waiting on a queue a thread of this run waits on, or a timer sends
// to, holds only this run's threads, each once, and ends; each such queue's buffer and indices are in
// range. A thread record of the host's own making (with a native context this run did not make) is
// refused here; the runtime stops the game if one is ever reached anyway.
bool runtime_structures_ok(const Image& image, const std::vector<uint32_t>& threads, const std::vector<uint32_t>& lists,
                           const std::vector<uint32_t>& queues, std::string* why) {
    const auto known = [&](uint32_t thread) { return std::find(threads.begin(), threads.end(), thread) != threads.end(); };
    // A list of waiting threads: this run's only, each once, and it ends.
    const auto list_ok = [&](uint32_t head) {
        uint32_t thread;
        if (!image.word(head, &thread)) {
            return false;
        }
        for (size_t count = 0; thread != 0; count++) {
            if (count >= threads.size() || !known(thread) ||
                !image.word(thread + static_cast<uint32_t>(offsetof(OSThread, next)), &thread)) {
                return false;
            }
        }
        return true;
    };
    // A message queue: its buffer in RDRAM, its indices inside it, both its lists as above.
    const auto queue_ok = [&](uint32_t queue) {
        uint32_t valid, first, count, msg;
        const auto field = [&](size_t offset, uint32_t* value) {
            return image.word(queue + static_cast<uint32_t>(offset), value);
        };
        if (!field(offsetof(OSMesgQueue, validCount), &valid) || !field(offsetof(OSMesgQueue, first), &first) ||
            !field(offsetof(OSMesgQueue, msgCount), &count) || !field(offsetof(OSMesgQueue, msg), &msg)) {
            return false;
        }
        const int32_t n = static_cast<int32_t>(count);
        const bool buffer = n == 0 || (n > 0 && msg % 4 == 0 && msg >= 0x80000000u &&
                                       uint64_t{msg} + 4ull * static_cast<uint64_t>(n) <= 0x80800000ull &&
                                       static_cast<int32_t>(first) >= 0 && static_cast<int32_t>(first) < n &&
                                       static_cast<int32_t>(valid) >= 0 && static_cast<int32_t>(valid) <= n);
        return buffer && list_ok(queue + static_cast<uint32_t>(offsetof(OSMesgQueue, blocked_on_recv))) &&
               list_ok(queue + static_cast<uint32_t>(offsetof(OSMesgQueue, blocked_on_send)));
    };
    for (const uint32_t head : lists) {
        // Where a snapshot is taken every thread but the running one waits to receive (snapshot.hpp), so the
        // list it waits in is its queue's first: the queue starts there. (Read as a queue's second list as
        // well, a changed queue's fields would pass as other ones.)
        if (!queue_ok(head)) {
            *why = "the message queue at " + hex(head) + " a thread waits on is not one the game can have made";
            return false;
        }
    }
    for (const uint32_t queue : queues) {
        if (!queue_ok(queue)) {
            *why = "the message queue at " + hex(queue) + " a timer sends to is not one the game can have made";
            return false;
        }
    }
    return true;
}

}  // namespace

void add(const char* name, void* data, size_t size, std::function<bool(const uint8_t*)> valid) {
    registered_fields().push_back({name,
                        [data, size] { return std::vector<uint8_t>(static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size); },
                        [data, size](const std::vector<uint8_t>& v) {
                            if (v.size() != size) return false;
                            std::memcpy(data, v.data(), size);
                            return true;
                        },
                        [size, valid = std::move(valid)](const std::vector<uint8_t>& v) {
                            return v.size() == size && (!valid || valid(v.data()));
                        }});
}

void add(const char* name, void* data, size_t size) { add(name, data, size, nullptr); }

void add_bools(const char* name, bool* data, size_t count) {
    add(name, data, count * sizeof(bool), [count](const uint8_t* bytes) {
        static_assert(sizeof(bool) == 1);
        return std::all_of(bytes, bytes + count, [](uint8_t b) { return b <= 1; });
    });
}

void add(const char* name, std::function<std::vector<uint8_t>()> save, std::function<bool(const std::vector<uint8_t>&)> load) {
    registered_fields().push_back({name, std::move(save), std::move(load), nullptr});
}

void add_check(std::function<const char*()> check) { checks().push_back(std::move(check)); }

void note_thread(recomp_context* ctx) {
    const PTR(OSThread) thread = ultramodern::this_thread();
    if (thread == NULLPTR) {
        return;  // the entry point, before any OS thread
    }
    std::lock_guard lock(g_threads_mutex);
    g_threads.push_back({thread, ctx});
}

recomp_context* thread_waiting_on(uint8_t* rdram, uint32_t queue) {
    std::lock_guard lock(g_threads_mutex);
    for (const Thread& t : g_threads) {
        if (place_of(rdram, t).queue == queue) {
            return t.ctx;
        }
    }
    return nullptr;
}

std::string describe_threads(uint8_t* rdram) {
    std::lock_guard lock(g_threads_mutex);
    std::string out;
    char line[96];
    for (const Thread& t : g_threads) {
        const Place p = place_of(rdram, t);
        std::snprintf(line, sizeof(line), " %08x:state %d queue %08x", p.thread, p.state, p.queue);
        out += line;
    }
    return out;
}

std::vector<uint8_t> save(uint8_t* rdram, recomp_context* ctx, uint32_t frame, std::string* why) {
    std::vector<Thread> threads;
    {
        std::lock_guard lock(g_threads_mutex);
        threads = g_threads;
    }
    if (const char* check = failed_check()) {
        *why = check;
        return {};
    }
    const PTR(OSThread) self = ultramodern::this_thread();
    Writer w;
    w.put(kMagic);
    w.put(kVersion);
    w.put(frame);
    w.put(static_cast<uint32_t>(threads.size()));
    for (const Thread& t : threads) {
        const bool running = t.thread == self;
        Place place = place_of(rdram, t);
        if (running) {
            place.ra = 0;
        }
        // Field by field: the struct's padding would carry whatever the stack held.
        w.put(place.thread);
        w.put(place.state);
        w.put(place.queue);
        w.put(place.ra);
        w.put(static_cast<uint8_t>(running));
        w.put(registers_of(running ? ctx : t.ctx));
    }
    w.bytes(rdram, kGameMemory);
    const auto timers = ultramodern::armed_timers_remaining(rdram);
    w.put(static_cast<uint32_t>(timers.size()));
    for (const auto& [timer, remaining] : timers) {
        w.put(timer);
        w.put(remaining);
    }
    std::vector<uint8_t> save_data(kSaveSize);
    save_read(rdram, static_cast<int32_t>(kSaveScratch), 0, kSaveSize);
    std::memcpy(save_data.data(), rdram + (kSaveScratch & 0x3FFFFFFF), kSaveSize);
    w.blob(save_data);
    w.put(static_cast<uint32_t>(fields().size()));
    for (const Field& field : fields()) {
        w.blob(std::vector<uint8_t>(field.name.begin(), field.name.end()));
        w.blob(field.save());
    }
    request_framebuffer_discard();
    return std::move(w.data);
}

uint32_t frame_of(const std::vector<uint8_t>& snapshot) {
    Reader r{snapshot};
    if (r.get<uint32_t>() != kMagic || r.get<uint32_t>() != kVersion) {
        return 0;
    }
    return r.get<uint32_t>();
}

bool load(uint8_t* rdram, recomp_context* ctx, const std::vector<uint8_t>& snapshot, std::string* why) {
    Reader r{snapshot};
    if (r.get<uint32_t>() != kMagic || r.get<uint32_t>() != kVersion) {
        *why = "not a snapshot of this version";
        return false;
    }
    r.get<uint32_t>();  // the frame
    if (const char* check = failed_check()) {
        *why = check;
        return false;
    }
    std::vector<Thread> threads;
    {
        std::lock_guard lock(g_threads_mutex);
        threads = g_threads;
    }
    const PTR(OSThread) self = ultramodern::this_thread();
    const uint32_t count = r.get<uint32_t>();
    if (!r.ok || count != threads.size()) {
        *why = "the snapshot has " + std::to_string(count) + " game threads, this run " + std::to_string(threads.size());
        return false;
    }
    std::vector<Registers> regs(count);
    std::vector<bool> running(count);
    for (uint32_t i = 0; i < count; i++) {
        Place theirs{};
        theirs.thread = r.get<uint32_t>();
        theirs.state = r.get<uint16_t>();
        theirs.queue = r.get<uint32_t>();
        theirs.ra = r.get<uint32_t>();
        running[i] = r.get<uint8_t>() != 0;
        regs[i] = r.get<Registers>();
        Place ours = place_of(rdram, threads[i]);
        const bool self_here = threads[i].thread == self;
        if (self_here) {
            ours.ra = 0;
        }
        if (!r.ok || theirs.thread != ours.thread || running[i] != self_here || theirs.state != ours.state ||
            theirs.queue != ours.queue || theirs.ra != ours.ra) {
            *why = "game thread " + std::to_string(i) + " (" + hex(ours.thread) + ") is at " + hex(ours.ra) + " on queue " +
                   hex(ours.queue) + " state " + std::to_string(ours.state) + ", in the snapshot at " + hex(theirs.ra) +
                   " on " + hex(theirs.queue) + " state " + std::to_string(theirs.state);
            return false;
        }
        // The snapshot comes from another machine: a register the recompiled code adds an offset to and
        // reads RDRAM through must be a 32-bit address, sign-extended, as every register of a game
        // compiled for 32-bit code holds. A 64-bit one would reach native memory outside RDRAM's
        // reserved 4 GiB (MEM_W is rdram + reg - 0xFFFFFFFF80000000).
        // The float mode is this build's (FR=1 for every game thread, game.cpp), not the snapshot's word.
        if (regs[i].mips3_float_mode != (self_here ? ctx : threads[i].ctx)->mips3_float_mode) {
            *why = "game thread " + std::to_string(i) + "'s float mode differs from this run's";
            return false;
        }
        for (int reg = 1; reg < 32; reg++) {
            if (regs[i].r[reg] != static_cast<gpr>(static_cast<int32_t>(regs[i].r[reg]))) {
                *why = "game thread " + std::to_string(i) + "'s register " + std::to_string(reg) + " is not a 32-bit value";
                return false;
            }
        }
    }
    const size_t memory_at = r.at;
    if (memory_at + kGameMemory > snapshot.size()) {
        *why = "the snapshot is cut short or from another build";
        return false;
    }
    const Image image{snapshot.data() + memory_at};
    r.at += kGameMemory;
    const uint32_t timer_count = r.get<uint32_t>();
    std::vector<std::pair<uint32_t, int64_t>> timers;
    for (uint32_t i = 0; i < timer_count && r.ok; i++) {
        const uint32_t timer = r.get<uint32_t>();
        const int64_t remaining = r.get<int64_t>();
        // An OSTimer in the game's memory, or nothing: the timer thread writes through it.
        if (r.ok && (timer < 0x80000000u || timer > 0x80000000u + kGameMemory - kOsTimerSize || timer % 8 != 0)) {
            *why = "a timer at " + hex(timer) + " is outside the game's memory";
            return false;
        }
        timers.emplace_back(timer, remaining);
    }
    // The queues the runtime will act on first: those this run's threads wait on (a thread's `queue` is
    // the list it waits in, at the head of its queue), and those the timers send to.
    std::vector<uint32_t> thread_addresses;
    std::vector<uint32_t> lists;
    std::vector<uint32_t> queues;
    for (const Thread& t : threads) {
        thread_addresses.push_back(static_cast<uint32_t>(t.thread));
        uint32_t queue = 0;  // the list it waits in: none, the running queue (a mark, not in RDRAM), or a queue's
        if (image.word(static_cast<uint32_t>(t.thread) + static_cast<uint32_t>(offsetof(OSThread, queue)), &queue) &&
            queue != 0 && queue != static_cast<uint32_t>(ultramodern::running_queue)) {
            lists.push_back(queue);
        }
    }
    for (const auto& timer : timers) {
        uint32_t mq = 0;
        if (image.word(timer.first + 24, &mq) && mq != 0) {  // OSTimer: next, prev, interval, value, then mq
            queues.push_back(mq);
        }
    }
    if (r.ok && !runtime_structures_ok(image, thread_addresses, lists, queues, why)) {
        return false;
    }
    const std::vector<uint8_t> save_data = r.blob();
    const uint32_t field_count = r.get<uint32_t>();
    std::vector<std::pair<std::string, std::vector<uint8_t>>> field_data;
    for (uint32_t i = 0; i < field_count && r.ok; i++) {
        const std::vector<uint8_t> name = r.blob();
        field_data.emplace_back(std::string(name.begin(), name.end()), r.blob());
    }
    if (!r.ok || save_data.size() != kSaveSize || field_count != fields().size()) {
        *why = "the snapshot is cut short or from another build";
        return false;
    }
    for (uint32_t i = 0; i < field_count; i++) {
        if (field_data[i].first != fields()[i].name) {
            *why = "the snapshot's native state differs from this build's (" + field_data[i].first + ")";
            return false;
        }
        // Its value, before the game's memory is touched: another machine's word.
        if (fields()[i].check && !fields()[i].check(field_data[i].second)) {
            *why = "the snapshot's native state " + field_data[i].first + " is not one this build makes";
            return false;
        }
    }
    // The game's memory, but each thread's native context pointer stays this run's.
    std::vector<UltraThreadContext*> contexts;
    for (const Thread& t : threads) {
        contexts.push_back(TO_PTR(OSThread, t.thread)->context);
    }
    std::memcpy(rdram, snapshot.data() + memory_at, kGameMemory);
    for (uint32_t i = 0; i < count; i++) {
        TO_PTR(OSThread, threads[i].thread)->context = contexts[i];
        set_registers(running[i] ? ctx : threads[i].ctx, regs[i]);
    }
    ultramodern::replace_timers(rdram, timers);
    std::memcpy(rdram + (kSaveScratch & 0x3FFFFFFF), save_data.data(), kSaveSize);
    save_write(rdram, static_cast<int32_t>(kSaveScratch), 0, kSaveSize);
    for (uint32_t i = 0; i < field_count; i++) {
        if (!fields()[i].load(field_data[i].second)) {
            *why = "native state " + field_data[i].first + " could not be loaded (the game's memory is already the snapshot's)";
            return false;
        }
    }
    g_discard_framebuffers.store(true);
    return true;
}

std::vector<std::string> self_test(uint8_t* rdram, recomp_context* ctx, uint32_t frame) {
    std::vector<std::string> out;
    std::string why;
    const std::vector<uint8_t> good = save(rdram, ctx, frame, &why);
    if (good.empty()) {
        out.push_back("FAIL no snapshot at this frame: " + why);
        return out;
    }
    // Where the game's memory starts in it, and a queue a thread of this run waits on.
    std::vector<Thread> threads;
    {
        std::lock_guard lock(g_threads_mutex);
        threads = g_threads;
    }
    const size_t memory_at = 16 + threads.size() * (4 + 2 + 4 + 4 + 1 + sizeof(Registers));
    const PTR(OSThread) self = ultramodern::this_thread();
    uint32_t queue = 0;
    uint32_t waiting = 0;
    for (const Thread& t : threads) {
        const OSThread* thread = TO_PTR(OSThread, t.thread);
        const uint32_t list = static_cast<uint32_t>(thread->queue);
        if (t.thread != self && list != 0 && list != static_cast<uint32_t>(ultramodern::running_queue)) {
            queue = list;  // at the latch every other thread waits to receive: the list is the queue's first
            waiting = static_cast<uint32_t>(t.thread);
            break;
        }
    }
    if (queue == 0) {
        out.push_back("FAIL no thread of this run waits on a queue");
        return out;
    }
    const auto at = [&](uint32_t address) { return memory_at + (address - 0x80000000u); };
    const auto put = [&](std::vector<uint8_t>& s, uint32_t address, uint32_t value) { std::memcpy(&s[at(address)], &value, 4); };
    const auto get = [&](const std::vector<uint8_t>& s, uint32_t address) {
        uint32_t value;
        std::memcpy(&value, &s[at(address)], 4);
        return value;
    };
    const uint32_t count_at = queue + static_cast<uint32_t>(offsetof(OSMesgQueue, msgCount));
    const uint32_t first_at = queue + static_cast<uint32_t>(offsetof(OSMesgQueue, first));
    const uint32_t msg_at = queue + static_cast<uint32_t>(offsetof(OSMesgQueue, msg));
    const uint32_t recv_at = queue + static_cast<uint32_t>(offsetof(OSMesgQueue, blocked_on_recv));
    const uint32_t next_offset = static_cast<uint32_t>(offsetof(OSThread, next));
    // A spare spot in the game's memory for a thread record of the host's making: a copy of one of this
    // run's, so the only thing wrong with it is where it is.
    const uint32_t stray = 0x80300000u;
    struct Case {
        const char* name;
        std::function<void(std::vector<uint8_t>&)> change;
    };
    const Case cases[] = {
        {"a queue with a negative slot count", [&](std::vector<uint8_t>& s) { put(s, count_at, 0xFFFFFFFFu); }},
        {"a queue whose first index is past its slots",
         [&](std::vector<uint8_t>& s) { put(s, first_at, get(s, count_at) + 0x10000000u); }},
        {"a queue whose buffer runs past RDRAM", [&](std::vector<uint8_t>& s) { put(s, msg_at, 0x807FFFFCu); }},
        {"a waiting thread that is not one of this run's",
         [&](std::vector<uint8_t>& s) {
             std::memcpy(&s[at(stray)], &s[at(waiting)], sizeof(OSThread));
             put(s, stray + next_offset, get(s, recv_at));
             put(s, recv_at, stray);
         }},
        {"a list of waiting threads that loops", [&](std::vector<uint8_t>& s) { put(s, waiting + next_offset, waiting); }},
    };
    const auto memory_hash = [&] {
        uint64_t hash = 0xCBF29CE484222325ull;
        for (uint32_t i = 0; i < kGameMemory; i++) {
            hash = (hash ^ rdram[i]) * 0x100000001B3ull;
        }
        return hash;
    };
    {
        const uint64_t before = memory_hash();
        std::string refused;
        const bool loaded = load(rdram, ctx, good, &refused);
        out.push_back(std::string(loaded && memory_hash() == before ? "PASS " : "FAIL ") +
                      "the snapshot unchanged: " + (loaded ? "loaded" : "refused (" + refused + ")"));
    }
    for (const Case& c : cases) {
        std::vector<uint8_t> changed = good;
        c.change(changed);
        const uint64_t before = memory_hash();
        std::string refused;
        const bool loaded = load(rdram, ctx, changed, &refused);
        const bool untouched = memory_hash() == before;
        out.push_back(std::string(!loaded && untouched ? "PASS " : "FAIL ") + c.name + ": " +
                      (loaded ? "loaded" : "refused (" + refused + ")") + (untouched ? "" : ", the game's memory changed"));
    }
    return out;
}

void request_framebuffer_discard() { g_discard_framebuffers.store(true); }

bool take_framebuffer_discard() { return g_discard_framebuffers.exchange(false); }

}  // namespace conker::snapshot
