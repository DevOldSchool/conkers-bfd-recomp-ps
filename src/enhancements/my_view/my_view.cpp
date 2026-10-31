// Online "my view" (docs/online-multiplayer-plan.md, M6): in a match each machine shows only its own
// player's view, scaled up to the window, and plays the sounds around that player as they hear
// them. Every machine still simulates and draws every view, which keeps the machines in step; only
// what is presented and the volume and pan the sound player is given change. Native helpers called
// from the [[patches.hook]] snippets in config/us.toml, and from the frame input hook.
//
// The picture. Rare's split screen keeps a record per view in D_800BE628 (0x180 bytes each: its rows
// at +0x24 top and +0x28 bottom, its columns at +0x2C and +0x30, its centre at +0x34 and +0x38), one
// more than the players for the whole screen, whose +0x4 and +0x8 are the frame buffer's width and
// height (292 x 216). Two players' views are 232 columns of the middle of the frame buffer, and a
// quarter is half its width less a border; RT64 widens a view to the whole width, or to its half
// (tools/patches/rt64-split-screen-wide.patch), so the part presented is the view's rows across the
// whole width or the half its centre lies in (its own columns when the picture is 4:3). The
// renderer presents only that part, scaled to fit the window (tools/patches/rt64-present-focus.patch).
// While the game is paused or shows any screen but play, the whole picture is presented as usual.
//
// The sounds. func_80011BB8 makes a listener per view in D_80041F68 (0x1C bytes: the camera, the
// ear, its yaw) from the views' camera records (*D_800DBFF0, 0x9A0 bytes each; a player's actor
// points at its own at +0x318). A positional sound takes its volume and pan from the nearest of
// them (func_800114D0 for the sounds kept in D_80041FE0, func_8000F6B8 for one-off sounds; path
// sounds, flag 0x40, from listener 0 alone through func_8000A750). A player's own sounds (an actor
// with a camera record) are not positional: everyone hears every player's at full volume, centred.
//
// What the game decides from a volume (whether a sound starts, is kept or freed) has to stay the
// same on every machine, so the game's calls are left as they are. Only the values handed to the
// sound player change: when a sound starts (sndp_play_sound) and when the game changes its volume
// or pan (sndp_post_event, types 8 and 4), they are worked out again for the local listener alone,
// by the game's own functions with the listener table holding only it, and other players' own
// sounds become positional around their characters. Once a frame, before the audio frame, a sound
// whose local volume or pan has moved while the game sent nothing gets new values posted, if the
// sound player's event queue has plenty of room left (a full queue drops events, which would change
// when sounds end). The sound player's volume and pan feed only the mix.
//
// CONKER_MY_VIEW=0 keeps the split screen and the shared sounds online. CONKER_MY_VIEW_PORT=<1-4>
// tries it outside a session, as that port's player (local split screen, for testing).
// CONKER_MY_VIEW_LOG=1 prints the view chosen and the sounds changed.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

#include "recomp.h"
#include "enhancements/my_view/my_view.hpp"
#include "enhancements/status.hpp"
#include "enhancements/switches.hpp"
#include "platform/display_mode.hpp"
#include "platform/present_focus.hpp"
#include "runtime/net_play.hpp"

extern "C" void func_800114D0(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_8000F6B8(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_8000A750(uint8_t* rdram, recomp_context* ctx);
extern "C" void sndp_post_event(uint8_t* rdram, recomp_context* ctx);

namespace {

inline gpr kseg0(uint32_t address) { return static_cast<gpr>(static_cast<int32_t>(address)); }
inline gpr word(int32_t value) { return static_cast<gpr>(value); }

constexpr uint32_t kMultiplayer = 0x800BE616;   // D_800BE616 (u8): a multiplayer match
constexpr uint32_t kScreenHandler = 0x800E0B94; // D_800E0B94 (u8): the frame's screen handler, 0 in play
constexpr uint32_t kPaused = 0x800BEAC0;        // D_800BEAC0 (u8): the pause is running
constexpr uint32_t kLastView = 0x80082FA0;      // D_80082FA0: views less one (0 outside the split screen)
constexpr uint32_t kViews = 0x800BE628;         // D_800BE628: the view records
constexpr uint32_t kViewSize = 0x180;
constexpr uint32_t kViewWidth = 0x4, kViewHeight = 0x8;  // f32, the whole screen's: the frame buffer
constexpr uint32_t kViewTop = 0x24, kViewBottom = 0x28, kViewLeft = 0x2C, kViewRight = 0x30;  // f32
constexpr uint32_t kViewCentreX = 0x34;  // f32
constexpr uint32_t kCameras = 0x800DBFF0;       // D_800DBFF0: the views' camera records
constexpr uint32_t kCameraSize = 0x9A0;
constexpr uint32_t kActors = 0x800CC2D0;        // player n is actor n in a match
constexpr uint32_t kActorSize = 0x32C;
constexpr uint32_t kActorAlive = 0x0;           // u32, 0 for a free actor
constexpr uint32_t kActorX = 0x14;              // f32 x, y, z
constexpr uint32_t kActorCamera = 0x318;        // the actor's camera record, players only

constexpr uint32_t kListeners = 0x80041F68;     // D_80041F68: a listener per view
constexpr uint32_t kListenerSize = 0x1C;
constexpr uint32_t kRecords = 0x80041FE0;       // D_80041FE0: the kept positional sounds
constexpr uint32_t kRecordCount = 0x80042760;   // D_80042760
constexpr uint32_t kRecordSize = 0x30;
constexpr uint32_t kHandles = 0x800425E0;       // D_800425E0: 16 sound handles
constexpr uint32_t kHandleSize = 0xC;
constexpr int kHandleCount = 16;
constexpr uint32_t kSoundPlayer = 0x8002BA2C;   // D_8002BA2C: the sound player
constexpr uint32_t kEventFreeList = 0x14;       // its event queue's free list (ALLink: next first)
constexpr uint32_t kStatePlaying = 0x54;        // u8 in a sound state, nonzero while it plays

std::atomic<bool> g_enabled{[] {
    const char* value = std::getenv("CONKER_MY_VIEW");
    return value == nullptr || value[0] != '0';
}()};  // the debug menu switches it

const int g_test_port = [] {
    const char* value = std::getenv("CONKER_MY_VIEW_PORT");
    const int port = value != nullptr ? std::atoi(value) : 0;
    return port >= 1 && port <= 4 ? port - 1 : -1;
}();

const bool g_log = [] {
    const char* value = std::getenv("CONKER_MY_VIEW_LOG");
    return value != nullptr && value[0] == '1';
}();

bool enabled() { return g_enabled.load(std::memory_order_relaxed); }

int local_port() {
    if (conker::net::active()) {
        return conker::net::local_port();
    }
    return g_test_port;
}

float read_float(uint8_t* rdram, uint32_t address) {
    const uint32_t bits = static_cast<uint32_t>(MEM_W(0, kseg0(address)));
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

bool in_rdram(uint32_t address) { return address >= 0x80000000u && address < 0x80800000u; }

// The local player's view in a split-screen match, or -1.
int local_view(uint8_t* rdram) {
    if (!enabled() || MEM_BU(0, kseg0(kMultiplayer)) == 0) {
        return -1;
    }
    const int port = local_port();
    const int32_t last = MEM_W(0, kseg0(kLastView));
    if (port < 0 || last < 1 || last > 3) {
        return -1;
    }
    // The view whose camera the port's character has, which is the port's own in Rare's order.
    const uint32_t cameras = static_cast<uint32_t>(MEM_W(0, kseg0(kCameras)));
    const uint32_t camera = static_cast<uint32_t>(MEM_W(kActorCamera, kseg0(kActors + port * kActorSize)));
    if (in_rdram(cameras) && camera >= cameras && (camera - cameras) % kCameraSize == 0) {
        const uint32_t view = (camera - cameras) / kCameraSize;
        if (view <= static_cast<uint32_t>(last)) {
            return static_cast<int>(view);
        }
    }
    return port <= last ? port : -1;
}

// ---- The picture ----

struct Focus {
    float x = 0.0f, y = 0.0f, width = 0.0f, height = 0.0f;
    bool operator==(const Focus& other) const {
        return x == other.x && y == other.y && width == other.width && height == other.height;
    }
};

Focus g_focus;

Focus view_focus(uint8_t* rdram, int view) {
    const uint32_t views = static_cast<uint32_t>(MEM_W(0, kseg0(kViews)));
    const int32_t last = MEM_W(0, kseg0(kLastView));
    if (!in_rdram(views)) {
        return {};
    }
    const uint32_t own = views + static_cast<uint32_t>(view) * kViewSize;
    const uint32_t whole = views + static_cast<uint32_t>(last + 1) * kViewSize;
    const float width = read_float(rdram, whole + kViewWidth);
    const float height = read_float(rdram, whole + kViewHeight);
    if (!(width > 16.0f) || !(height > 16.0f)) {
        return {};
    }
    Focus focus;
    const float centre = read_float(rdram, own + kViewCentreX) / width;
    if (!conker::display::rendered_widescreen()) {
        focus.x = read_float(rdram, own + kViewLeft) / width;
        focus.width = (read_float(rdram, own + kViewRight) - read_float(rdram, own + kViewLeft)) / width;
    } else if (centre < 0.45f) {
        focus.width = 0.5f;
    } else if (centre > 0.55f) {
        focus.x = 0.5f;
        focus.width = 0.5f;
    } else {
        focus.width = 1.0f;
    }
    focus.y = read_float(rdram, own + kViewTop) / height;
    focus.height = (read_float(rdram, own + kViewBottom) - read_float(rdram, own + kViewTop)) / height;
    if (!(focus.x >= 0.0f) || !(focus.width > 0.1f) || focus.x + focus.width > 1.001f || !(focus.y >= 0.0f) ||
        !(focus.height > 0.1f) || focus.y + focus.height > 1.001f) {
        return {};
    }
    return focus;
}

void present(uint8_t* rdram, int view) {
    Focus focus;
    const bool playing = MEM_BU(0, kseg0(kScreenHandler)) == 0 && MEM_BU(0, kseg0(kPaused)) == 0;
    if (view >= 0 && playing) {
        focus = view_focus(rdram, view);
    }
    if (focus == g_focus) {
        return;
    }
    g_focus = focus;
    conker::renderer::set_present_focus(focus.x, focus.y, focus.width, focus.height);
    if (g_log) {
        std::printf("[conker] my view: view %d presents %.3f %.3f %.3f %.3f\n", view, focus.x, focus.y, focus.width,
                    focus.height);
        std::fflush(stdout);
    }
}

// ---- The sounds ----

// Who a sound belongs to, to work its volume and pan out again.
enum class Kind : uint8_t {
    Kept,   // a sound in D_80041FE0 (func_80011624), found again by its handle
    Spot,   // a one-off sound at a point (func_80010E78)
    Owner,  // another player's own sound, at their character
};

// A positional query as the game made it.
struct Query {
    bool path = false;  // func_8000A750 (flag 0x40), else func_800114D0
    int32_t x = 0, y = 0, z = 0;  // for a path sound, y is the path
    int32_t near_distance = 0, far_distance = 0;
    bool pan = true;  // flag 0x100 keeps a kept sound centred
};

struct Source {
    Kind kind = Kind::Kept;
    Query query;          // Kept until the record has the handle; Spot: the point
    uint32_t owner = 0;   // Owner: the actor
    int32_t base = 0;     // Spot and Owner: the volume at full strength
    int32_t sent_volume = -1;
    int32_t sent_pan = -1;
};

struct Pending {
    bool set = false;
    Source source;
};

std::unordered_map<uint16_t, Source> g_sources;  // by sound handle
Query g_query;
bool g_query_set = false;
Pending g_spot;       // from func_80010E78's arguments
Pending g_owner;      // from an actor's sound function's arguments
bool g_owner_centred = false;  // set by func_80010630, which starts through func_80010F30
Pending g_marked;     // the sound a caller of func_80010BE8 is about to start
Pending g_start;      // the sound func_80010BE8 is starting
Pending g_play;       // the sound func_80010BE8 hands sndp_play_sound
bool g_posting = false;  // the refresh's own events
int g_view = -1;      // the local view while a match runs, for the hooks

// A context for calling the game's functions from a hook, with a stack frame well below the hook's.
recomp_context call_context(const recomp_context* ctx, gpr* sp) {
    recomp_context c = *ctx;
    *sp = (ctx->r29 - 0x300) & ~gpr{0xF};
    c.r29 = *sp;
    return c;
}

// The stack below a hook's, put back as it was after the calls: what lies there is left over from
// the game's own calls and may be read again (a local the game never set), and this machine's calls
// are not every machine's.
class StackGuard {
public:
    StackGuard(uint8_t* rdram, const recomp_context* ctx) : rdram_(rdram), top_(ctx->r29 & ~gpr{0x3}) {
        for (uint32_t i = 0; i < kWords; i++) {
            saved_[i] = MEM_W(-static_cast<int32_t>((i + 1) * 4), top_);
        }
    }
    ~StackGuard() {
        uint8_t* rdram = rdram_;
        for (uint32_t i = 0; i < kWords; i++) {
            MEM_W(-static_cast<int32_t>((i + 1) * 4), top_) = saved_[i];
        }
    }

private:
    static constexpr uint32_t kWords = 0x1000 / 4;
    uint8_t* rdram_;
    gpr top_;
    int32_t saved_[kWords];
};

// The volume (of 0x7FFF) and pan the game gives a sound of the query, from every listener or from
// the local one alone (the listener table briefly holds only it).
struct Heard {
    int32_t volume = 0;
    int32_t pan = 0x40;  // with 0x80 for the effects mix, as the game keeps it
};

Heard ask(uint8_t* rdram, const recomp_context* ctx, const Query& query, bool local) {
    gpr sp;
    recomp_context c = call_context(ctx, &sp);
    const uint32_t saved_last = static_cast<uint32_t>(MEM_W(0, kseg0(kLastView)));
    uint32_t saved[kListenerSize / 4];
    const uint32_t listener = local ? kListeners + static_cast<uint32_t>(g_view) * kListenerSize : kListeners;
    if (local) {
        for (uint32_t i = 0; i < kListenerSize / 4; i++) {
            saved[i] = static_cast<uint32_t>(MEM_W(i * 4, kseg0(kListeners)));
        }
        for (uint32_t i = 0; i < kListenerSize / 4; i++) {
            MEM_W(i * 4, kseg0(kListeners)) = MEM_W(i * 4, kseg0(listener));
        }
        MEM_W(0, kseg0(kLastView)) = 0;
    }
    Heard heard;
    const gpr pan = sp + 0x40, volume = sp + 0x44, distance = sp + 0x48;
    MEM_W(0, pan) = 0x40;
    MEM_W(0, volume) = 0;
    if (query.path) {
        // As func_80011624 asks for a path sound: the listener's ear, yaw and camera by value.
        c.r4 = word(query.y);
        c.r5 = word(MEM_W(0xC, kseg0(listener)));
        c.r6 = word(MEM_W(0x10, kseg0(listener)));
        c.r7 = word(MEM_W(0x14, kseg0(listener)));
        MEM_W(0x10, sp) = MEM_W(0x18, kseg0(listener));
        MEM_W(0x14, sp) = MEM_W(0x0, kseg0(listener));
        MEM_W(0x18, sp) = MEM_W(0x4, kseg0(listener));
        MEM_W(0x1C, sp) = MEM_W(0x8, kseg0(listener));
        MEM_W(0x20, sp) = query.near_distance;
        MEM_W(0x24, sp) = query.far_distance;
        MEM_W(0x28, sp) = static_cast<int32_t>(pan);
        MEM_W(0x2C, sp) = static_cast<int32_t>(volume);
        MEM_W(0x30, sp) = static_cast<int32_t>(distance);
        func_8000A750(rdram, &c);
        heard.volume = MEM_W(0, volume);
    } else {
        c.r4 = word(query.x);
        c.r5 = word(query.y);
        c.r6 = word(query.z);
        c.r7 = word(0x7FFF);
        MEM_W(0x10, sp) = query.near_distance;
        MEM_W(0x14, sp) = query.far_distance;
        MEM_W(0x18, sp) = static_cast<int32_t>(pan);
        MEM_W(0x1C, sp) = static_cast<int32_t>(volume);
        MEM_W(0x20, sp) = static_cast<int32_t>(distance);
        func_800114D0(rdram, &c);
        heard.volume = MEM_W(0, volume);
    }
    heard.pan = MEM_W(0, pan);
    if (local) {
        for (uint32_t i = 0; i < kListenerSize / 4; i++) {
            MEM_W(i * 4, kseg0(kListeners)) = static_cast<int32_t>(saved[i]);
        }
        MEM_W(0, kseg0(kLastView)) = static_cast<int32_t>(saved_last);
    }
    return heard;
}

// A sound at a point as func_8000F6B8 hears it from the local listener alone.
Heard ask_point(uint8_t* rdram, const recomp_context* ctx, const Query& query) {
    gpr sp;
    recomp_context c = call_context(ctx, &sp);
    const uint32_t saved_last = static_cast<uint32_t>(MEM_W(0, kseg0(kLastView)));
    uint32_t saved[kListenerSize / 4];
    const uint32_t listener = kListeners + static_cast<uint32_t>(g_view) * kListenerSize;
    for (uint32_t i = 0; i < kListenerSize / 4; i++) {
        saved[i] = static_cast<uint32_t>(MEM_W(i * 4, kseg0(kListeners)));
    }
    for (uint32_t i = 0; i < kListenerSize / 4; i++) {
        MEM_W(i * 4, kseg0(kListeners)) = MEM_W(i * 4, kseg0(listener));
    }
    MEM_W(0, kseg0(kLastView)) = 0;
    const gpr pan = sp + 0x40;
    MEM_W(0, pan) = 0x40;
    c.r4 = 0;
    c.r5 = word(static_cast<int16_t>(query.x));
    c.r6 = word(static_cast<int16_t>(query.y));
    c.r7 = word(static_cast<int16_t>(query.z));
    MEM_W(0x10, sp) = static_cast<int32_t>(pan);
    MEM_W(0x14, sp) = static_cast<int16_t>(query.near_distance);
    MEM_W(0x18, sp) = static_cast<int16_t>(query.far_distance);
    func_8000F6B8(rdram, &c);
    Heard heard;
    heard.volume = static_cast<int32_t>(c.r2);
    heard.pan = MEM_W(0, pan);
    for (uint32_t i = 0; i < kListenerSize / 4; i++) {
        MEM_W(i * 4, kseg0(kListeners)) = static_cast<int32_t>(saved[i]);
    }
    MEM_W(0, kseg0(kLastView)) = static_cast<int32_t>(saved_last);
    return heard;
}

int32_t flipped(int32_t pan) {
    const int32_t value = 0x80 - pan;
    return value == 0x80 ? 0x7F : value;
}

// The kept sound with this handle in D_80041FE0, or 0.
uint32_t kept_record(uint8_t* rdram, uint16_t handle) {
    const int32_t count = std::min<int32_t>(MEM_W(0, kseg0(kRecordCount)), 0x20);
    for (int32_t i = 0; i < count; i++) {
        const uint32_t record = kRecords + static_cast<uint32_t>(i) * kRecordSize;
        if (MEM_HU(0x24, kseg0(record)) == handle && (static_cast<uint32_t>(MEM_W(0x10, kseg0(record))) & 0x80) == 0) {
            return record;
        }
    }
    return 0;
}

Query record_query(uint8_t* rdram, uint32_t record) {
    const uint32_t flags = static_cast<uint32_t>(MEM_W(0x10, kseg0(record)));
    Query query;
    query.path = (flags & 0x40) != 0;
    query.x = MEM_H(0x2, kseg0(record));
    query.y = MEM_H(0x4, kseg0(record));
    query.z = MEM_H(0x6, kseg0(record));
    query.near_distance = MEM_HU(0x8, kseg0(record));
    query.far_distance = MEM_H(0xA, kseg0(record));
    query.pan = (flags & 0x100) == 0;
    return query;
}

// The volume and pan for the local listener of a sound the game gives game_volume and game_pan.
// For a kept sound the game's volume is scaled by how much quieter it is here than at the nearest
// listener, which keeps whatever its callback did to it.
struct Local {
    bool valid = false;
    int32_t volume = 0;
    int32_t pan = -1;  // -1: keep the game's
};

Local local_sound(uint8_t* rdram, const recomp_context* ctx, Source& source, uint16_t handle, int32_t game_volume,
                  int32_t game_pan) {
    Local local;
    switch (source.kind) {
        case Kind::Kept: {
            const uint32_t record = kept_record(rdram, handle);
            const Query query = record != 0 ? record_query(rdram, record) : source.query;
            const Heard everyone = ask(rdram, ctx, query, false);
            const Heard here = ask(rdram, ctx, query, true);
            if (everyone.volume <= 0) {
                return local;
            }
            local.valid = true;
            local.volume = static_cast<int32_t>(std::min<int64_t>(0x7FFF, int64_t{game_volume} * here.volume / everyone.volume));
            if (query.pan && game_pan >= 0) {
                if (game_pan == (everyone.pan & 0x7F)) {
                    local.pan = here.pan & 0x7F;
                } else if (game_pan == flipped(everyone.pan & 0x7F)) {
                    local.pan = flipped(here.pan & 0x7F);
                }
            }
            return local;
        }
        case Kind::Spot: {
            const Heard here = ask_point(rdram, ctx, source.query);
            local.valid = true;
            local.volume = (source.base * here.volume) >> 15;
            local.pan = here.pan & 0x7F;
            return local;
        }
        case Kind::Owner: {
            if (MEM_W(kActorAlive, kseg0(source.owner)) == 0) {
                return local;
            }
            Query query = source.query;
            query.x = static_cast<int32_t>(read_float(rdram, source.owner + kActorX));
            query.y = static_cast<int32_t>(read_float(rdram, source.owner + kActorX + 4));
            query.z = static_cast<int32_t>(read_float(rdram, source.owner + kActorX + 8));
            const Heard here = ask_point(rdram, ctx, query);
            local.valid = true;
            local.volume = (source.base * here.volume) >> 15;
            local.pan = here.pan & 0x7F;
            return local;
        }
    }
    return local;
}

// The handle whose sound state this is, or 0.
uint16_t handle_of(uint8_t* rdram, uint32_t state) {
    for (int i = 0; i < kHandleCount; i++) {
        const uint32_t entry = kHandles + static_cast<uint32_t>(i) * kHandleSize;
        if (static_cast<uint32_t>(MEM_W(8, kseg0(entry))) == state) {
            return MEM_HU(0, kseg0(entry));
        }
    }
    return 0;
}

bool sounds_active() { return g_view >= 0; }

// Whether an actor's own sound is another player's: a player (it has a camera record) not here.
bool other_player(uint8_t* rdram, uint32_t owner) {
    if (!in_rdram(owner) || MEM_W(kActorAlive, kseg0(owner)) == 0) {
        return false;
    }
    const uint32_t camera = static_cast<uint32_t>(MEM_W(kActorCamera, kseg0(owner)));
    const uint32_t cameras = static_cast<uint32_t>(MEM_W(0, kseg0(kCameras)));
    return camera != 0 && camera != cameras + static_cast<uint32_t>(g_view) * kCameraSize;
}

void set_owner(uint8_t* rdram, uint32_t owner, int32_t near_distance, int32_t far_distance, bool centred) {
    g_owner.set = false;
    if (!sounds_active() || !other_player(rdram, owner)) {
        return;
    }
    g_owner.set = true;
    g_owner.source = {};
    g_owner.source.kind = Kind::Owner;
    g_owner.source.owner = owner;
    g_owner.source.query.near_distance = near_distance;
    g_owner.source.query.far_distance = far_distance;
    g_owner_centred = centred;
}

// Once a frame: new volumes and pans for sounds the local listener hears differently now, posted
// while the sound player's event queue has room to spare.
void refresh(uint8_t* rdram, recomp_context* ctx) {
    if (!sounds_active()) {
        g_sources.clear();
        return;
    }
    const uint32_t player = static_cast<uint32_t>(MEM_W(0, kseg0(kSoundPlayer)));
    if (!in_rdram(player)) {
        return;
    }
    int free_events = 0;
    for (uint32_t link = static_cast<uint32_t>(MEM_W(kEventFreeList, kseg0(player))); in_rdram(link) && free_events < 1024;
         link = static_cast<uint32_t>(MEM_W(0, kseg0(link)))) {
        free_events++;
    }
    constexpr int kSpareEvents = 24;  // left free for what the sound player posts itself this frame
    StackGuard guard(rdram, ctx);
    std::unordered_map<uint16_t, Source> live;
    for (int i = 0; i < kHandleCount; i++) {
        const uint32_t entry = kHandles + static_cast<uint32_t>(i) * kHandleSize;
        const uint16_t handle = MEM_HU(0, kseg0(entry));
        const uint32_t state = static_cast<uint32_t>(MEM_W(8, kseg0(entry)));
        const auto it = g_sources.find(handle);
        if (it == g_sources.end() || !in_rdram(state)) {
            continue;
        }
        Source source = it->second;
        if (MEM_BU(kStatePlaying, kseg0(state)) != 0) {
            int32_t game_volume = source.sent_volume;
            int32_t game_pan = -1;
            if (source.kind == Kind::Kept) {
                const uint32_t record = kept_record(rdram, handle);
                if (record != 0) {
                    game_volume = static_cast<uint16_t>(MEM_H(0x26, kseg0(record)));
                    game_pan = MEM_BU(0x22, kseg0(record)) & 0x7F;
                }
            }
            const Local local = local_sound(rdram, ctx, source, handle, game_volume, game_pan);
            if (local.valid) {
                const int32_t step = std::max(64, source.sent_volume / 16);
                const bool volume_moved = source.sent_volume < 0 || std::abs(local.volume - source.sent_volume) > step ||
                                          ((local.volume == 0) != (source.sent_volume == 0));
                const bool pan_moved = local.pan >= 0 && local.pan != source.sent_pan;
                const int wanted = (volume_moved ? 1 : 0) + (pan_moved ? 1 : 0);
                if (wanted > 0 && free_events - wanted >= kSpareEvents) {
                    g_posting = true;
                    gpr sp;
                    if (volume_moved) {
                        recomp_context c = call_context(ctx, &sp);
                        c.r4 = kseg0(state);
                        c.r5 = 8;
                        c.r6 = word(local.volume);
                        sndp_post_event(rdram, &c);
                        source.sent_volume = local.volume;
                    }
                    if (pan_moved) {
                        recomp_context c = call_context(ctx, &sp);
                        c.r4 = kseg0(state);
                        c.r5 = 4;
                        c.r6 = word(local.pan);
                        sndp_post_event(rdram, &c);
                        source.sent_pan = local.pan;
                    }
                    g_posting = false;
                    free_events -= wanted;
                }
            }
        }
        live.emplace(handle, source);
    }
    g_sources.swap(live);
}

}  // namespace

void conker::my_view::frame(uint8_t* rdram, recomp_context* ctx) {
    g_marked.set = g_start.set = g_play.set = false;
    g_view = local_view(rdram);
    present(rdram, g_view);
    refresh(rdram, ctx);
}

// func_800114D0's entry, as func_80011624 asks where a kept sound is heard: its query.
extern "C" void conker_my_view_kept_query(uint8_t* rdram, recomp_context* ctx) {
    if (!sounds_active()) {
        return;
    }
    g_query = {};
    g_query.x = static_cast<int32_t>(ctx->r4);
    g_query.y = static_cast<int32_t>(ctx->r5);
    g_query.z = static_cast<int32_t>(ctx->r6);
    g_query.near_distance = MEM_W(0x10, ctx->r29);
    g_query.far_distance = MEM_W(0x14, ctx->r29);
    g_query.pan = MEM_W(0x18, ctx->r29) != 0;
    g_query_set = true;
}

// func_8000A750's entry, as func_80011624 asks for a path sound (flag 0x40).
extern "C" void conker_my_view_path_query(uint8_t* rdram, recomp_context* ctx) {
    if (!sounds_active()) {
        return;
    }
    g_query = {};
    g_query.path = true;
    g_query.y = static_cast<int32_t>(ctx->r4);
    g_query.near_distance = MEM_W(0x20, ctx->r29);
    g_query.far_distance = MEM_W(0x24, ctx->r29);
    g_query_set = true;
}

// func_80010E78's entry: a one-off sound at a point (handle, sound, volume, cents, mix, -, x, y, z,
// near, far).
extern "C" void conker_my_view_spot(uint8_t* rdram, recomp_context* ctx) {
    g_spot.set = false;
    if (!sounds_active()) {
        return;
    }
    g_spot.set = true;
    g_spot.source = {};
    g_spot.source.kind = Kind::Spot;
    g_spot.source.base = static_cast<uint16_t>(ctx->r6);
    g_spot.source.query.x = MEM_W(0x18, ctx->r29);
    g_spot.source.query.y = MEM_W(0x1C, ctx->r29);
    g_spot.source.query.z = MEM_W(0x20, ctx->r29);
    g_spot.source.query.near_distance = MEM_W(0x24, ctx->r29);
    g_spot.source.query.far_distance = MEM_W(0x28, ctx->r29);
}

// func_8000F91C's entry: a one-off sound moved (handle, volume, cents, mix, -, x, y, z, near, far).
extern "C" void conker_my_view_spot_moved(uint8_t* rdram, recomp_context* ctx) {
    const auto it = g_sources.find(static_cast<uint16_t>(ctx->r4));
    if (!sounds_active() || it == g_sources.end() || it->second.kind != Kind::Spot) {
        return;
    }
    Source& source = it->second;
    source.base = static_cast<uint16_t>(ctx->r5);
    source.query.x = MEM_W(0x14, ctx->r29);
    source.query.y = MEM_W(0x18, ctx->r29);
    source.query.z = MEM_W(0x1C, ctx->r29);
    source.query.near_distance = MEM_W(0x20, ctx->r29);
    source.query.far_distance = MEM_W(0x24, ctx->r29);
}

// The entry of func_80010154, func_80010344 and func_80010630 (sound, actor, volume, near, far):
// an actor's sound, centred and at full volume when the actor is a player.
extern "C" void conker_my_view_owner(uint8_t* rdram, recomp_context* ctx) {
    set_owner(rdram, static_cast<uint32_t>(ctx->r5), static_cast<int16_t>(ctx->r7),
              static_cast<uint16_t>(MEM_W(0x10, ctx->r29)), false);
}

extern "C" void conker_my_view_owner_centred(uint8_t* rdram, recomp_context* ctx) {
    set_owner(rdram, static_cast<uint32_t>(ctx->r5), static_cast<int16_t>(ctx->r7),
              static_cast<uint16_t>(MEM_W(0x10, ctx->r29)), true);
}

// func_80010FFC's entry (handle, sound, volume, cents, mix, actor): as for an actor that is no
// player, its sound fades out between 500 and 2,501 units.
extern "C" void conker_my_view_owner_handle(uint8_t* rdram, recomp_context* ctx) {
    set_owner(rdram, static_cast<uint32_t>(MEM_W(0x14, ctx->r29)), 0x1F4, 0x9C5, false);
}

// Just before a call to func_80010BE8 (recompiled calls leave no return address to tell callers
// apart): which of the above the sound it starts is. 0: func_80011624's kept sound; 1: func_80010E78's
// sound at a point; 2: an actor's own sound (func_80010154, func_80010344, func_80010FFC); 3:
// func_80010F30's centred sound, an actor's when func_80010630 called it.
extern "C" void conker_my_view_mark(uint8_t* /*rdram*/, recomp_context* /*ctx*/, int caller) {
    g_marked.set = false;
    if (!sounds_active()) {
        return;
    }
    if (caller == 0 && g_query_set) {
        g_marked.set = true;
        g_marked.source = {};
        g_marked.source.kind = Kind::Kept;
        g_marked.source.query = g_query;
    } else if (caller == 1 && g_spot.set) {
        g_marked = g_spot;
    } else if ((caller == 2 && g_owner.set && !g_owner_centred) || (caller == 3 && g_owner.set && g_owner_centred)) {
        g_marked = g_owner;
    }
    g_query_set = false;
    g_spot.set = false;
    g_owner.set = false;
}

// func_80010BE8's entry: the sound it starts, if its caller said.
extern "C" void conker_my_view_start(uint8_t* /*rdram*/, recomp_context* /*ctx*/) {
    g_start = g_marked;
    g_marked.set = false;
}

// func_80010BE8, just before it calls sndp_play_sound.
extern "C" void conker_my_view_starting(uint8_t* /*rdram*/, recomp_context* /*ctx*/) {
    g_play = g_start;
    g_start.set = false;
}

// sndp_play_sound's entry (player, sound, volume, pan, pitch, mix, bus, handle's state slot): the
// local volume and pan in place of the game's, for a sound func_80010BE8 starts.
extern "C" void conker_my_view_play(uint8_t* rdram, recomp_context* ctx) {
    if (!g_play.set) {
        return;
    }
    Source source = g_play.source;
    g_play.set = false;
    const uint32_t slot = static_cast<uint32_t>(MEM_W(0x1C, ctx->r29));
    if (slot < kHandles + 8 || slot >= kHandles + kHandleCount * kHandleSize || (slot - kHandles - 8) % kHandleSize != 0) {
        return;
    }
    const uint16_t handle = MEM_HU(0, kseg0(slot - 8));
    const int32_t game_volume = static_cast<uint16_t>(ctx->r6);
    const int32_t game_pan = static_cast<uint8_t>(ctx->r7);
    if (source.kind == Kind::Owner) {
        source.base = game_volume;
    }
    Local local;
    {
        StackGuard guard(rdram, ctx);
        local = local_sound(rdram, ctx, source, handle, game_volume, game_pan);
    }
    if (!local.valid) {
        return;
    }
    ctx->r6 = word(local.volume);
    source.sent_volume = local.volume;
    if (local.pan >= 0) {
        ctx->r7 = word(local.pan);
        source.sent_pan = local.pan;
    } else {
        source.sent_pan = game_pan;
    }
    g_sources[handle] = source;
    if (g_log) {
        std::printf("[conker] my view: sound %u (handle 0x%X, %s) volume %d -> %d, pan %d -> %d\n",
                    static_cast<unsigned>(ctx->r5 & 0xFFFF), handle,
                    source.kind == Kind::Kept ? "kept" : source.kind == Kind::Spot ? "spot" : "player", game_volume,
                    local.volume, game_pan, static_cast<int>(ctx->r7 & 0xFF));
        std::fflush(stdout);
    }
}

// sndp_post_event's entry (state, type, value): the game changing a sound's volume (8) or pan (4)
// gets the local listener's.
extern "C" void conker_my_view_post(uint8_t* rdram, recomp_context* ctx) {
    const int16_t type = static_cast<int16_t>(ctx->r5);
    if (g_posting || !sounds_active() || (type != 8 && type != 4)) {
        return;
    }
    const uint16_t handle = handle_of(rdram, static_cast<uint32_t>(ctx->r4));
    const auto it = g_sources.find(handle);
    if (handle == 0 || it == g_sources.end()) {
        return;
    }
    Source& source = it->second;
    const int32_t value = static_cast<int32_t>(ctx->r6);
    if (type == 8 && source.kind == Kind::Owner) {
        source.base = value;
    }
    int32_t game_volume = type == 8 ? value : source.sent_volume;
    int32_t game_pan = type == 4 ? value : -1;
    if (source.kind == Kind::Kept && type == 4) {
        const uint32_t record = kept_record(rdram, handle);
        game_volume = record != 0 ? static_cast<uint16_t>(MEM_H(0x26, kseg0(record))) : source.sent_volume;
    }
    Local local;
    {
        StackGuard guard(rdram, ctx);
        local = local_sound(rdram, ctx, source, handle, game_volume, game_pan);
    }
    if (!local.valid) {
        return;
    }
    if (type == 8) {
        ctx->r6 = word(local.volume);
        source.sent_volume = local.volume;
    } else if (local.pan >= 0) {
        ctx->r6 = word(local.pan);
        source.sent_pan = local.pan;
    }
}

nlohmann::json conker::status::my_view() {
    return enhancement("my_view", "Online: only your own view and sounds", enabled(), "CONKER_MY_VIEW");
}

conker::switches::Switch conker::switches::my_view() {
    return {"MY VIEW", enabled, [](bool on) { g_enabled.store(on); }, true};
}
