// Online "my view" (docs/online-multiplayer-plan.md, M6): in a match each machine shows only its own
// player's view, scaled up to the window, and plays the sounds around that player as they hear
// them. Every machine still simulates and draws every view, which keeps the machines in step; only
// what is presented and the volume and pan the sound player is given change. Native helpers called
// from the [[patches.hook]] snippets in config/us.toml, and from the frame input hook.
//
// The picture. Every machine draws the split screen as the game does, which keeps the frame's
// memory the same everywhere. Then the views are drawn again for the local player alone, as the game
// draws a match with one pad against bots: one view over the whole screen, with one player's HUD
// (draw_local_view). That pass starts from the game's memory as it was where the views started
// drawing, with the local camera as the game's own drawing left it, and the renderer draws the frame
// from the memory the pass leaves (draw_next_frame_from); the game's memory is then put back as the
// game left it. While the pass runs, no other thread runs and nothing from outside is delivered
// (tools/patches/n64modernruntime-isolated-pass.patch), and the fixed step's osGetCount reads are
// put back too, so the game goes on exactly as on every other machine. The pause and every screen
// but play show the split screen as usual. CONKER_MY_VIEW_FULL=0 crops the local player's split-screen
// view instead (D_800BE628's records: rows +0x24/+0x28, columns +0x2C/+0x30; the record after the
// players' is the whole screen, the frame buffer's size at +0x4/+0x8), presented scaled to fit the
// window (tools/patches/rt64-present-focus.patch).
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
// whose volume or pan has moved in some view while the game sent nothing gets new values posted, if
// the sound player's event queue has plenty of room left (a full queue drops events, which would
// change when sounds end). The sound player's volume and pan feed only the mix, so the audio memory
// below 0x80082B20 differs between machines (the desync hash leaves it out), but every machine has
// to post the same events: each one takes an event and a synth update param, and a machine that ran
// short of either would drop work the others did (a voice not freed, a sound bank not released,
// in the hashed heap). So every machine of a session follows every view's sounds, whatever its own
// MY VIEW switch or view, decides from all views' values when to post, and posts its own view's
// values (the game's where it has none). CONKER_AUDIO_COUNT_LOG compares the counts.
//
// CONKER_MY_VIEW=0 keeps the split screen and the shared sounds online. CONKER_MY_VIEW_PORT=<1-4>
// tries it outside a session, as that port's player (local split screen, for testing).
// CONKER_MY_VIEW_LOG=1 prints the view chosen and the sounds changed.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

#include "recomp.h"
#include "enhancements/my_view/my_view.hpp"
#include "enhancements/status.hpp"
#include "enhancements/switches.hpp"
#include "platform/display_mode.hpp"
#include "platform/present_focus.hpp"
#include "net/net_play.hpp"
#include "runtime/frame_input.hpp"

extern "C" void func_800114D0(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_8000F6B8(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_8000A750(uint8_t* rdram, recomp_context* ctx);
extern "C" void sndp_post_event(uint8_t* rdram, recomp_context* ctx);
// The frame's drawing, as func_1501878C calls it (see draw_local_view).
extern "C" void func_1501A8C0(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_1501B22C(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_1510B128(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_1501B640(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_15172D80(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_15019464(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_1515D6D0(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_150195A0(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_15019414(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_150198FC(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_15019CC8(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_15019BB8(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_15019E60(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_151E84B0(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_15043384(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_150347E8(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_1502E474(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_15189118(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_15084A18(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_15125628(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_80004674(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_1510D404(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_15114A1C(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_1517CFC4(uint8_t* rdram, recomp_context* ctx);

extern "C" bool recomp_dma_done_now;
extern "C" bool ultramodern_isolated;

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
constexpr uint32_t kViewMatrices = 0x800DC2A0;   // D_800DC2A0: two buffers of a matrix per view
constexpr uint32_t kViewRecords = 0x800DC2B0;    // D_800DC2B0: 0xB0 bytes per view
constexpr uint32_t kCameraCopies = 0x800DC010;   // D_800DC010: a second 0x9A0 bytes per view
constexpr uint32_t kViewBuffers = 0x800BE2B0;    // D_800BE2B0: a buffer per view
constexpr uint32_t kViewFlags = 0x800DBFF4;      // D_800DBFF4: a byte per view
constexpr uint32_t kActors = 0x800CC2D0;        // player n is actor n in a match
constexpr uint32_t kActorSize = 0x32C;
constexpr uint32_t kActorAlive = 0x0;           // u32, 0 for a free actor
constexpr uint32_t kActorX = 0x14;              // f32 x, y, z
constexpr uint32_t kActorCamera = 0x318;        // the actor's camera record, players only
constexpr uint32_t kPlayerActor = 0x80088720;   // D_80088720: the player's actor (player 1's in a match)

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

// CONKER_MY_VIEW_FULL=0: the local view cut out of the split screen instead of drawn again whole.
const bool g_full = [] {
    const char* value = std::getenv("CONKER_MY_VIEW_FULL");
    return value == nullptr || value[0] != '0';
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

// The views whose sounds are followed: every view of a split-screen match in an online session
// (or while CONKER_MY_VIEW_PORT tries it), on every machine alike, so that every machine posts the
// same events to the sound player whichever view it hears from.
int sound_views(uint8_t* rdram) {
    if (!conker::net::active() && g_test_port < 0) {
        return 0;
    }
    if (MEM_BU(0, kseg0(kMultiplayer)) == 0) {
        return 0;
    }
    const int32_t last = MEM_W(0, kseg0(kLastView));
    return last >= 1 && last <= 3 ? last + 1 : 0;
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
    if (view >= 0 && playing && !g_full) {
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

constexpr int kMaxViews = 4;

struct Source {
    Kind kind = Kind::Kept;
    Query query;          // Kept until the record has the handle; Spot: the point
    uint32_t owner = 0;   // Owner: the actor
    int32_t base = 0;     // Spot and Owner: the volume at full strength
    // What each view was sent last (index view + 1; 0 the game's own), -1 before anything was.
    int32_t sent_volume[kMaxViews + 1] = {-1, -1, -1, -1, -1};
    int32_t sent_pan[kMaxViews + 1] = {-1, -1, -1, -1, -1};
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
int g_view = -1;      // the local view while a match runs, for the hooks (-1: none, or MY VIEW off)
// The match's views while sounds are followed, the same on every machine of a session whatever its
// own MY VIEW switch or port: 0 for none.
int g_sound_views = 0;
bool g_drawing = false;  // the local view is being drawn again

// A context for calling the game's functions from a hook, with a stack frame well below the hook's.
recomp_context call_context(const recomp_context* ctx, gpr* sp) {
    recomp_context c = *ctx;
    *sp = (ctx->r29 - 0x300) & ~gpr{0xF};
    c.r29 = *sp;
    return c;
}

// A copied context's odd float registers still point into the one it was copied from (f_odd):
// point them at its own, as the runtime does for a context of its own.
void own_float_registers(recomp_context& c) {
    c.f_odd = c.mips3_float_mode ? &c.f1.u32l : &c.f0.u32h;
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

// The volume (of 0x7FFF) and pan the game gives a sound of the query, from every listener (view -1,
// as the game itself asks) or from one view's alone (the listener table briefly holds only it).
struct Heard {
    int32_t volume = 0;
    int32_t pan = 0x40;  // with 0x80 for the effects mix, as the game keeps it
};

// While a query is asked for one view: the listener table holds only that view's listener.
class OneListener {
public:
    OneListener(uint8_t* rdram, int view) : rdram_(rdram), view_(view) {
        if (view_ < 0) {
            return;
        }
        saved_last_ = static_cast<uint32_t>(MEM_W(0, kseg0(kLastView)));
        const uint32_t listener = kListeners + static_cast<uint32_t>(view_) * kListenerSize;
        for (uint32_t i = 0; i < kListenerSize / 4; i++) {
            saved_[i] = static_cast<uint32_t>(MEM_W(i * 4, kseg0(kListeners)));
        }
        for (uint32_t i = 0; i < kListenerSize / 4; i++) {
            MEM_W(i * 4, kseg0(kListeners)) = MEM_W(i * 4, kseg0(listener));
        }
        MEM_W(0, kseg0(kLastView)) = 0;
    }
    ~OneListener() {
        if (view_ < 0) {
            return;
        }
        uint8_t* rdram = rdram_;
        for (uint32_t i = 0; i < kListenerSize / 4; i++) {
            MEM_W(i * 4, kseg0(kListeners)) = static_cast<int32_t>(saved_[i]);
        }
        MEM_W(0, kseg0(kLastView)) = static_cast<int32_t>(saved_last_);
    }
    // Listener 0 while the query runs.
    uint32_t listener() const { return kListeners; }

private:
    uint8_t* rdram_;
    int view_;
    uint32_t saved_last_ = 0;
    uint32_t saved_[kListenerSize / 4] = {};
};

Heard ask(uint8_t* rdram, const recomp_context* ctx, const Query& query, int view) {
    gpr sp;
    recomp_context c = call_context(ctx, &sp);
    own_float_registers(c);
    const OneListener only(rdram, view);
    const uint32_t listener = only.listener();
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
    return heard;
}

// A sound at a point as func_8000F6B8 hears it, from every listener (view -1) or one view's alone.
Heard ask_point(uint8_t* rdram, const recomp_context* ctx, const Query& query, int view) {
    gpr sp;
    recomp_context c = call_context(ctx, &sp);
    own_float_registers(c);
    const OneListener only(rdram, view);
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

// The volume and pan one view hears a sound at that the game gives game_volume and game_pan (view
// -1: the game's own, as every view together hears it). For a kept sound the game's volume is
// scaled by how much quieter it is in the view than at the nearest listener, which keeps whatever
// its callback did to it. Whether it is valid never depends on the view, so every machine follows
// the same sounds.
struct Local {
    bool valid = false;
    int32_t volume = 0;
    int32_t pan = -1;  // -1: keep the game's
};

bool is_own_player(uint8_t* rdram, uint32_t owner, int view) {
    const uint32_t camera = static_cast<uint32_t>(MEM_W(kActorCamera, kseg0(owner)));
    const uint32_t cameras = static_cast<uint32_t>(MEM_W(0, kseg0(kCameras)));
    return camera == cameras + static_cast<uint32_t>(view) * kCameraSize;
}

Local local_sound(uint8_t* rdram, const recomp_context* ctx, const Source& source, uint16_t handle, int32_t game_volume,
                  int32_t game_pan, int view) {
    Local local;
    switch (source.kind) {
        case Kind::Kept: {
            const uint32_t record = kept_record(rdram, handle);
            const Query query = record != 0 ? record_query(rdram, record) : source.query;
            const Heard everyone = ask(rdram, ctx, query, -1);
            if (everyone.volume <= 0) {
                return local;
            }
            local.valid = true;
            if (view < 0) {
                local.volume = game_volume;
                return local;
            }
            const Heard here = ask(rdram, ctx, query, view);
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
            const Heard here = ask_point(rdram, ctx, source.query, view);
            local.valid = true;
            local.volume = (source.base * here.volume) >> 15;
            local.pan = here.pan & 0x7F;
            return local;
        }
        case Kind::Owner: {
            if (MEM_W(kActorAlive, kseg0(source.owner)) == 0) {
                return local;
            }
            local.valid = true;
            if (view < 0 || is_own_player(rdram, source.owner, view)) {
                local.volume = source.base;  // as the game plays a player's own sound, with its pan
                return local;
            }
            Query query = source.query;
            query.x = static_cast<int32_t>(read_float(rdram, source.owner + kActorX));
            query.y = static_cast<int32_t>(read_float(rdram, source.owner + kActorX + 4));
            query.z = static_cast<int32_t>(read_float(rdram, source.owner + kActorX + 8));
            const Heard here = ask_point(rdram, ctx, query, view);
            local.volume = (source.base * here.volume) >> 15;
            local.pan = here.pan & 0x7F;
            return local;
        }
    }
    return local;
}

// Every view's values (index view + 1), the game's own first.
struct Locals {
    Local view[kMaxViews + 1];
};

Locals local_sounds(uint8_t* rdram, const recomp_context* ctx, const Source& source, uint16_t handle, int32_t game_volume,
                    int32_t game_pan) {
    Locals locals;
    for (int view = -1; view < g_sound_views; view++) {
        locals.view[view + 1] = local_sound(rdram, ctx, source, handle, game_volume, game_pan, view);
    }
    return locals;
}

// This machine's values of the view's: the game's own when it has no view (or shows the split
// screen), and the game's pan where the view keeps it.
int local_index() { return g_view >= 0 && g_view < g_sound_views ? g_view + 1 : 0; }

// What every view was last sent: the view's own pan where it has one, else the game's.
void record_sent(Source& source, const Locals& locals, bool volume, bool pan, int32_t game_pan) {
    for (int i = 0; i <= g_sound_views; i++) {
        if (volume) {
            source.sent_volume[i] = locals.view[i].volume;
        }
        if (pan) {
            source.sent_pan[i] = locals.view[i].pan >= 0 ? locals.view[i].pan : game_pan;
        }
    }
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

// (Asked again of the session: leaving one mid-frame ends the sounds' following at once.)
bool sounds_active() { return g_sound_views > 0 && !g_drawing && (conker::net::active() || g_test_port >= 0); }

// Whether an actor's own sound is a player's (it has a camera record), heard positionally in every
// other player's view.
bool player_owned(uint8_t* rdram, uint32_t owner) {
    if (!in_rdram(owner) || MEM_W(kActorAlive, kseg0(owner)) == 0) {
        return false;
    }
    return MEM_W(kActorCamera, kseg0(owner)) != 0;
}

void set_owner(uint8_t* rdram, uint32_t owner, int32_t near_distance, int32_t far_distance, bool centred) {
    g_owner.set = false;
    if (!sounds_active() || !player_owned(rdram, owner)) {
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

// Once a frame: new volumes and pans for sounds a view hears differently now, posted while the
// sound player's event queue has room to spare. Whether an event is posted is decided from every
// view's values, so every machine posts the same events and only their values differ.
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
    const int here = local_index();
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
            int32_t game_volume = source.sent_volume[0];
            int32_t game_pan = -1;
            if (source.kind == Kind::Kept) {
                const uint32_t record = kept_record(rdram, handle);
                if (record != 0) {
                    game_volume = static_cast<uint16_t>(MEM_H(0x26, kseg0(record)));
                    game_pan = MEM_BU(0x22, kseg0(record)) & 0x7F;
                }
            }
            const Locals locals = local_sounds(rdram, ctx, source, handle, game_volume, game_pan);
            if (locals.view[0].valid) {
                bool volume_moved = false, pan_moved = false;
                for (int v = 0; v <= g_sound_views; v++) {
                    const Local& local = locals.view[v];
                    const int32_t sent = source.sent_volume[v];
                    const int32_t step = std::max(64, sent / 16);
                    volume_moved = volume_moved || sent < 0 || std::abs(local.volume - sent) > step ||
                                   ((local.volume == 0) != (sent == 0));
                    pan_moved = pan_moved || (local.pan >= 0 && local.pan != source.sent_pan[v]);
                }
                const int wanted = (volume_moved ? 1 : 0) + (pan_moved ? 1 : 0);
                if (wanted > 0 && free_events - wanted >= kSpareEvents) {
                    const int32_t kept_pan = game_pan >= 0 ? game_pan : source.sent_pan[0];
                    g_posting = true;
                    gpr sp;
                    if (volume_moved) {
                        recomp_context c = call_context(ctx, &sp);
                        own_float_registers(c);
                        c.r4 = kseg0(state);
                        c.r5 = 8;
                        c.r6 = word(locals.view[here].volume);
                        sndp_post_event(rdram, &c);
                    }
                    if (pan_moved) {
                        const int32_t pan = locals.view[here].pan >= 0 ? locals.view[here].pan
                                            : source.sent_pan[here] >= 0 ? source.sent_pan[here]
                                            : kept_pan >= 0 ? kept_pan : 0x40;
                        recomp_context c = call_context(ctx, &sp);
                        own_float_registers(c);
                        c.r4 = kseg0(state);
                        c.r5 = 4;
                        c.r6 = word(pan);
                        sndp_post_event(rdram, &c);
                    }
                    g_posting = false;
                    record_sent(source, locals, volume_moved, pan_moved, kept_pan);
                    free_events -= wanted;
                }
            }
        }
        live.emplace(handle, source);
    }
    g_sources.swap(live);
}

// ---- The local view drawn whole ----

constexpr uint32_t kViewOrder = 0x800BE630;     // D_800BE630 (u8[]): the view each pass of the loops draws
constexpr uint32_t kCurrentView = 0x80082FA4;   // D_80082FA4: the view being drawn
constexpr uint32_t kListBase = 0x800BE9C8;      // D_800BE9C8: the two display list buffers
constexpr uint32_t kListIndex = 0x800BE9C0;     // D_800BE9C0 (u8): the frame's one
constexpr uint32_t kNoWorld = 0x800BEBA0;       // D_800BEBA0 (u8): skips the world's set-up commands
constexpr uint32_t kWorldImage = 0x800BEB9C;    // D_800BEB9C: the frame's colour image
constexpr uint32_t kWorldMatrices = 0x800D9B90; // D_800D9B90: two matrices a list buffer
constexpr uint32_t kViewSetUp = 0x8003C8E0;     // D_8003C8E0: set while a view is set up
constexpr uint32_t kFarClip = 0x800D3674;       // D_800D3674 (s16): from the camera's +0x388
constexpr uint32_t kGameMemory = 0x400000;      // the game's 4 MiB
constexpr uint32_t kImageSize = 0x800000;       // all of RDRAM, as the renderer reads it
constexpr uint32_t kHiddenColour = 0x80600000;  // where the views before the local one draw, unseen
constexpr uint32_t kHiddenDepth = 0x80640000;

gpr g_list_pointer = 0;     // where func_1501878C keeps the display list pointer ($s1)
uint32_t g_list_start = 0;  // where the views' commands start in the list
std::vector<uint8_t> g_before_views;  // game memory where func_1501878C starts drawing the views
// The HUD's text queue (func_15042ECC's nodes, drawn by func_15043384): a 0x5C-byte node each, with
// the 3D object it is drawn over (or none), its position and its next node.
constexpr uint32_t kTexts = 0x800CBD64;  // D_800CBD64: the first node
constexpr uint32_t kTextAnchor = 0x0, kTextX = 0x8, kTextY = 0xA, kTextNext = 0x18;
// func_151DDC20 queues the match timer at (0x87, 0x64), between the split views, or at (0x87, 0xC6)
// with one view.
constexpr int16_t kTimerX = 0x87, kSplitTimerY = 0x64, kOneViewTimerY = 0xC6;
uint32_t g_count_reads_before = 0;    // and the frame's osGetCount reads then
bool g_holding = false;               // conker::renderer::hold_game_memory from there to the pass's end

void let_go_of_memory() {
    if (g_holding) {
        g_holding = false;
        conker::renderer::release_game_memory();
    }
}

// Calls one of the frame's drawing functions; returns $v0.
uint32_t call(uint8_t* rdram, const recomp_context* ctx, void (*function)(uint8_t*, recomp_context*), gpr a0 = 0, gpr a1 = 0,
              gpr a2 = 0, gpr a3 = 0) {
    gpr sp;
    recomp_context c = call_context(ctx, &sp);
    own_float_registers(c);
    c.r4 = a0;
    c.r5 = a1;
    c.r6 = a2;
    c.r7 = a3;
    function(rdram, &c);
    return static_cast<uint32_t>(c.r2);
}

uint32_t list_pointer(uint8_t* rdram) { return static_cast<uint32_t>(MEM_W(0, g_list_pointer)); }
void set_list_pointer(uint8_t* rdram, uint32_t value) { MEM_W(0, g_list_pointer) = static_cast<int32_t>(value); }
// A drawing function that takes the list in $a0 and returns its new end.
void draw(uint8_t* rdram, const recomp_context* ctx, void (*function)(uint8_t*, recomp_context*), gpr a1 = 0) {
    set_list_pointer(rdram, call(rdram, ctx, function, kseg0(list_pointer(rdram)), a1));
}

void emit(uint8_t* rdram, uint32_t w0, uint32_t w1) {
    const uint32_t at = list_pointer(rdram);
    MEM_W(0, kseg0(at)) = static_cast<int32_t>(w0);
    MEM_W(4, kseg0(at)) = static_cast<int32_t>(w1);
    set_list_pointer(rdram, at + 8);
}

void write_float(uint8_t* rdram, uint32_t address, float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    MEM_W(0, kseg0(address)) = static_cast<int32_t>(bits);
}

struct Rect {
    float left = 0.0f, top = 0.0f, right = 0.0f, bottom = 0.0f;
    bool holds(float x, float y) const { return x >= left && x < right && y >= top && y < bottom; }
};

Rect view_rect(uint8_t* rdram, uint32_t record) {
    return {read_float(rdram, record + kViewLeft), read_float(rdram, record + kViewTop), read_float(rdram, record + kViewRight),
            read_float(rdram, record + kViewBottom)};
}

// The split screen's views, as the game laid them out for this frame.
std::vector<Rect> split_views(uint8_t* rdram) {
    std::vector<Rect> rects;
    const uint32_t views = static_cast<uint32_t>(MEM_W(0, kseg0(kViews)));
    const int32_t last = MEM_W(0, kseg0(kLastView));
    for (int32_t view = 0; in_rdram(views) && view <= last && view < 4; view++) {
        rects.push_back(view_rect(rdram, views + static_cast<uint32_t>(view) * kViewSize));
    }
    return rects;
}

// The HUD text the game queued outside the views' drawing, for the split screen: each player's lines
// at places in their own view, the timer between the views. Drawn again for the local player alone,
// the other players' lines go (they showed over this player's view) and the local player's keep their
// distance from the nearer side of the view in each direction, now of the whole screen; the timer goes
// where a one-view match has it. Lines over a 3D object are placed when drawn, from the camera.
void place_queued_texts(uint8_t* rdram, const std::vector<Rect>& split, int local, const Rect& whole) {
    uint32_t node = static_cast<uint32_t>(MEM_W(0, kseg0(kTexts)));
    for (int count = 0; count < 256 && in_rdram(node); count++, node = static_cast<uint32_t>(MEM_W(kTextNext, kseg0(node)))) {
        if (MEM_W(kTextAnchor, kseg0(node)) != 0) {
            continue;
        }
        const int16_t x = MEM_H(kTextX, kseg0(node));
        const int16_t y = MEM_H(kTextY, kseg0(node));
        if (x == kTimerX && y == kSplitTimerY) {
            MEM_H(kTextY, kseg0(node)) = kOneViewTimerY;
            continue;
        }
        for (size_t view = 0; view < split.size(); view++) {
            const Rect& in = split[view];
            if (!in.holds(x, y)) {
                continue;
            }
            if (view != static_cast<size_t>(local)) {
                MEM_H(kTextY, kseg0(node)) = -4000;
                break;
            }
            const float new_x = x - in.left < in.right - x ? whole.left + (x - in.left) : whole.right - (in.right - x);
            const float new_y = y - in.top < in.bottom - y ? whole.top + (y - in.top) : whole.bottom - (in.bottom - y);
            MEM_H(kTextX, kseg0(node)) = static_cast<int16_t>(new_x);
            MEM_H(kTextY, kseg0(node)) = static_cast<int16_t>(new_y);
            break;
        }
    }
}

// A view's field of view as its camera set it this frame with func_1510B128(view, x, y, scale, fov):
// the angles +0x74 and +0x78 are the view's own (+0x6C, +0x70) and the camera's change (a gun's
// scope zooms with it) and D_800C3648; +0x1C is +0x7C * +0x14 * cot(+0x74) * +0x0C * scale.
// func_1501A8C0 lays the view out again with no change, so a zoom is put back after it.
constexpr uint32_t kViewAngleOffset = 0x800C3648;  // D_800C3648 (f32)
constexpr uint32_t kAngleToRadians = 0x800A2C24;   // D_800A2C24 (f32): +0x74 to the angle func_1510B128 takes
struct Zoom {
    bool zoomed = false;
    float x = 0.0f, y = 0.0f, scale = 1.0f;
};

Zoom zoom_of(uint8_t* rdram, int view) {
    const uint32_t record = static_cast<uint32_t>(MEM_W(0, kseg0(kViews))) + static_cast<uint32_t>(view) * kViewSize;
    const float offset = read_float(rdram, kViewAngleOffset);
    Zoom zoom;
    zoom.x = read_float(rdram, record + 0x74) - read_float(rdram, record + 0x6C) - offset;
    zoom.y = read_float(rdram, record + 0x78) - read_float(rdram, record + 0x70) - offset;
    const float angle = read_float(rdram, record + 0x74) * read_float(rdram, kAngleToRadians);
    const float unscaled = read_float(rdram, record + 0x7C) * read_float(rdram, record + 0x14) *
                           (std::cos(angle) * read_float(rdram, record + 0xC) / std::sin(angle));
    if (std::isfinite(unscaled) && std::fabs(unscaled) > 1e-6f) {
        const float scale = read_float(rdram, record + 0x1C) / unscaled;
        zoom.scale = std::isfinite(scale) && scale > 0.0f ? scale : 1.0f;
    }
    zoom.zoomed = std::fabs(zoom.x) > 1e-3f || std::fabs(zoom.y) > 1e-3f || std::fabs(zoom.scale - 1.0f) > 1e-3f;
    return zoom;
}

gpr float_word(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return word(static_cast<int32_t>(bits));
}

void apply_zoom(uint8_t* rdram, const recomp_context* ctx, int view, const Zoom& zoom) {
    gpr sp;
    recomp_context c = call_context(ctx, &sp);
    own_float_registers(c);
    c.r4 = word(static_cast<int16_t>(view));
    c.r5 = float_word(zoom.x);
    c.r6 = float_word(zoom.y);
    c.r7 = float_word(zoom.scale);
    MEM_W(0x10, sp) = 0;  // fov: the view's own
    func_1510B128(rdram, &c);
}

// The views' part of the frame, as func_1501878C draws it from 0x15018938 to 0x15018CDC, again, with
// the game set up as for one player (as with one pad against bots): one view, the local player's,
// over the whole screen. The other views' records are moved off the screen, as the HUD draws each
// player's pie from its own. The camera is where the game's own drawing stepped it this frame (put
// in place by the caller), so it is not stepped again.
void draw_local_view(uint8_t* rdram, recomp_context* ctx, int local) {
    const gpr view = word(static_cast<int16_t>(local));
    const std::vector<Rect> split = split_views(rdram);
    MEM_W(0, kseg0(kLastView)) = 0;
    MEM_BU(0, kseg0(kViewOrder)) = static_cast<uint8_t>(local);
    const Zoom zoom = zoom_of(rdram, local);
    call(rdram, ctx, func_1501A8C0, view, 0, 0x3FF, 0);  // the local view's record over the whole screen
    if (zoom.zoomed) {
        apply_zoom(rdram, ctx, local, zoom);
    }
    call(rdram, ctx, func_1501B22C, view);  // its view planes, from its new field of view (props at the sides)
    const uint32_t views = static_cast<uint32_t>(MEM_W(0, kseg0(kViews)));
    // The other views' records become the local one's: the HUD sizes every player's text from view
    // 0's and places it by the view it was queued for. The views before the local one are moved off
    // the screen, as the HUD draws their players' pies from them.
    const uint32_t own = views + static_cast<uint32_t>(local) * kViewSize;
    for (uint32_t other = 0; other < 4; other++) {
        if (other == static_cast<uint32_t>(local)) {
            continue;
        }
        const uint32_t record = views + other * kViewSize;
        for (uint32_t field = kViewTop; field <= kViewCentreX + 4; field += 4) {
            write_float(rdram, record + field, read_float(rdram, own + field) + (other < static_cast<uint32_t>(local) ? 4000.0f : 0.0f));
        }
        write_float(rdram, record + kViewWidth, read_float(rdram, own + kViewWidth));
        write_float(rdram, record + kViewHeight, read_float(rdram, own + kViewHeight));
    }
    set_list_pointer(rdram, g_list_start);
    place_queued_texts(rdram, split, local, view_rect(rdram, own));

    // The views before the local one are set up and drawn as the game draws them, in the game's order:
    // the game builds some of a frame's geometry once, as the first view that needs it draws, and
    // later views use it (P2's character came out as a jumble without them). Their world goes to a
    // colour and depth image of its own above the game's memory, so it is not seen.
    uint32_t colour_image = 0, depth_image = 0;  // the frame's, as its list set them before the views
    {
        const uint32_t start = static_cast<uint32_t>(MEM_W(4 * MEM_BU(0, kseg0(kListIndex)), kseg0(kListBase)));
        for (uint32_t at = start; at < g_list_start; at += 8) {
            const uint32_t w0 = static_cast<uint32_t>(MEM_W(0, kseg0(at)));
            if ((w0 >> 24) == 0xFF) colour_image = at;
            if ((w0 >> 24) == 0xFE) depth_image = at;
        }
    }
    auto before_local = [&](auto&& body) {
        if (local > 0) {
            body();
        }
    };
    before_local([&] {
        for (int other = 0; other < local; other++) {
            MEM_W(0, kseg0(kCurrentView)) = other;
            MEM_W(0, kseg0(kViewSetUp)) = 0x0A000000;
            call(rdram, ctx, func_1501B640);
            call(rdram, ctx, func_15172D80, word(other));
            MEM_W(0, kseg0(kViewSetUp)) = 0;
            draw(rdram, ctx, func_15019464, word(other));
        }
    });

    // func_15018F80 without the camera's step (func_1517F448).
    MEM_W(0, kseg0(kCurrentView)) = local;
    MEM_W(0, kseg0(kViewSetUp)) = 0x0A000000;
    call(rdram, ctx, func_1501B640);
    const uint32_t camera = static_cast<uint32_t>(MEM_W(0, kseg0(kCameras))) + static_cast<uint32_t>(local) * kCameraSize;
    MEM_H(0, kseg0(kFarClip)) = read_float(rdram, camera + 0x388) < 0.0f ? -0x3E8 : 0x1388;
    call(rdram, ctx, func_15172D80, view);
    MEM_W(0, kseg0(kViewSetUp)) = 0;
    const uint32_t local_world = list_pointer(rdram);
    draw(rdram, ctx, func_15019464, view);
    // The scissor the local view's world drew with: the views before it are drawn again next, their
    // records off the screen (an empty scissor), and with one view func_150195A0 does not set its
    // view's scissor again, so the local view's items (weapons in hand, pickups) and HUD came out
    // clipped away.
    uint32_t scissor_w0 = 0;
    uint32_t scissor_w1 = 0;
    for (uint32_t at = local_world; at < list_pointer(rdram); at += 8) {
        if ((static_cast<uint32_t>(MEM_W(0, kseg0(at))) >> 24) == 0xED) {
            scissor_w0 = static_cast<uint32_t>(MEM_W(0, kseg0(at)));
            scissor_w1 = static_cast<uint32_t>(MEM_W(4, kseg0(at)));
        }
    }

    if (MEM_BU(0, kseg0(kNoWorld)) == 0) {
        const uint32_t matrices = kWorldMatrices + (static_cast<uint32_t>(MEM_BU(0, kseg0(kListIndex))) << 5);
        emit(rdram, 0xE1000000, static_cast<uint32_t>(MEM_W(0, kseg0(kWorldImage))));
        emit(rdram, 0xDD0007FF, 0x800C1060);
        emit(rdram, 0xD9C2F9FA, 0);
        emit(rdram, 0xD9FFFFFF, 0x00220005);
        emit(rdram, 0xDA380003, 0x80089470);
        emit(rdram, 0xDC08000A, matrices);
        emit(rdram, 0xDC08060A, matrices + 0x10);
    }
    before_local([&] {
        const bool images = colour_image != 0 && depth_image != 0;
        if (images) {
            emit(rdram, static_cast<uint32_t>(MEM_W(0, kseg0(colour_image))), kHiddenColour);
            emit(rdram, 0xFE000000, kHiddenDepth);
        }
        for (int other = 0; other < local; other++) {
            MEM_W(0, kseg0(kCurrentView)) = other;
            draw(rdram, ctx, func_1515D6D0, word(other));
            draw(rdram, ctx, func_150195A0, word(static_cast<int16_t>(other)));
        }
        if (images) {
            emit(rdram, static_cast<uint32_t>(MEM_W(0, kseg0(colour_image))), static_cast<uint32_t>(MEM_W(4, kseg0(colour_image))));
            emit(rdram, static_cast<uint32_t>(MEM_W(0, kseg0(depth_image))), static_cast<uint32_t>(MEM_W(4, kseg0(depth_image))));
        }
    });
    if (local > 0 && scissor_w0 != 0) {
        emit(rdram, scissor_w0, scissor_w1);
    }
    MEM_W(0, kseg0(kCurrentView)) = local;
    draw(rdram, ctx, func_1515D6D0, word(local));
    draw(rdram, ctx, func_150195A0, view);

    // The HUD draws a pie for each view up to the count: up to the local one, the ones before it off
    // the screen.
    MEM_W(0, kseg0(kLastView)) = local;
    for (int other = 0; other <= local; other++) {
        MEM_BU(other, kseg0(kViewOrder)) = static_cast<uint8_t>(other);
    }
    MEM_W(0, kseg0(kCurrentView)) = 0;
    call(rdram, ctx, func_15019414);
    draw(rdram, ctx, func_150198FC);
    MEM_W(0, kseg0(kCurrentView)) = local;
    draw(rdram, ctx, func_1515D6D0, word(local));
    draw(rdram, ctx, func_15019CC8, word(local));
    draw(rdram, ctx, func_15019E60);
    if (MEM_BU(0, kseg0(kPaused)) == 0) {
        MEM_W(0, kseg0(kCurrentView)) = local;
        draw(rdram, ctx, func_15019BB8, word(local));
    }
    draw(rdram, ctx, func_151E84B0);
    draw(rdram, ctx, func_15043384);
    call(rdram, ctx, func_150347E8);
    call(rdram, ctx, func_1502E474);  // the frame's bone matrices, made fixed point
    call(rdram, ctx, func_15189118, 0);
    call(rdram, ctx, func_15084A18);
    call(rdram, ctx, func_15125628);
    emit(rdram, 0xE9000000, 0);  // G_RDPFULLSYNC
    emit(rdram, 0xDF000000, 0);  // G_ENDDL
    // What func_1501878C does after ending the list and before sending it.
    call(rdram, ctx, func_80004674);
    call(rdram, ctx, func_1510D404);
    call(rdram, ctx, func_15114A1C);
    call(rdram, ctx, func_1517CFC4);
}

}  // namespace

void conker::my_view::frame(uint8_t* rdram, recomp_context* ctx) {
    g_marked.set = g_start.set = g_play.set = false;
    g_view = local_view(rdram);
    g_sound_views = sound_views(rdram);
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
    Locals locals;
    {
        StackGuard guard(rdram, ctx);
        locals = local_sounds(rdram, ctx, source, handle, game_volume, game_pan);
    }
    if (!locals.view[0].valid) {
        return;
    }
    const Local& local = locals.view[local_index()];
    ctx->r6 = word(local.volume);
    if (local.pan >= 0) {
        ctx->r7 = word(local.pan);
    }
    record_sent(source, locals, true, true, game_pan);
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
// gets the local view's.
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
    int32_t game_volume = type == 8 ? value : source.sent_volume[0];
    int32_t game_pan = type == 4 ? value : -1;
    if (source.kind == Kind::Kept && type == 4) {
        const uint32_t record = kept_record(rdram, handle);
        game_volume = record != 0 ? static_cast<uint16_t>(MEM_H(0x26, kseg0(record))) : source.sent_volume[0];
    }
    Locals locals;
    {
        StackGuard guard(rdram, ctx);
        locals = local_sounds(rdram, ctx, source, handle, game_volume, game_pan);
    }
    if (!locals.view[0].valid) {
        return;
    }
    const Local& local = locals.view[local_index()];
    if (type == 8) {
        ctx->r6 = word(local.volume);
    } else if (local.pan >= 0) {
        ctx->r6 = word(local.pan);
    }
    record_sent(source, locals, type == 8, type == 4, game_pan);
}

// CONKER_MY_VIEW_DUMP=<folder>: every 150 frames, the game's memory with its own split-screen list
// (f<frame>.game) and with the pass's list (f<frame>.mine), big-endian as the RSP reads it; the last
// 8 of each are kept. Walking both lists from the list start the log gives shows what the pass draws
// differently (docs/diagnostics.md).
void dump_pass(const uint8_t* game, const uint8_t* mine) {
    static const char* const folder = std::getenv("CONKER_MY_VIEW_DUMP");
    if (folder == nullptr) {
        return;
    }
    const uint32_t frame = conker::frame_input::current().frame;
    static uint32_t last = 0;
    if (last != 0 && frame < last + 150) {
        return;
    }
    last = frame;
    auto write = [&](const uint8_t* image, uint32_t at, const char* suffix) {
        const std::string path = std::string(folder) + "/f" + std::to_string(at) + suffix;
        if (frame == at) {
            if (FILE* f = std::fopen(path.c_str(), "wb")) {
                std::vector<uint8_t> out(kGameMemory);
                for (uint32_t a = 0; a < kGameMemory; a += 4) {
                    uint32_t word;
                    std::memcpy(&word, image + a, 4);
                    out[a] = uint8_t(word >> 24);
                    out[a + 1] = uint8_t(word >> 16);
                    out[a + 2] = uint8_t(word >> 8);
                    out[a + 3] = uint8_t(word);
                }
                std::fwrite(out.data(), 1, out.size(), f);
                std::fclose(f);
            }
        } else {
            std::remove(path.c_str());
        }
    };
    static std::deque<uint32_t> kept;
    kept.push_back(frame);
    write(game, frame, ".game");
    write(mine, frame, ".mine");
    while (kept.size() > 8) {
        write(nullptr, kept.front(), ".game");
        write(nullptr, kept.front(), ".mine");
        kept.pop_front();
    }
    std::printf("[conker] my view: dumped frame %u (list start 0x%08X, view %d)\n", frame, g_list_start, g_view);
}

// func_1501878C at 0x15018938, where it starts drawing the views: the display list pointer's
// address ($s1), where the views' commands start, and the game's memory as it is then.
extern "C" void conker_my_view_draw_start(uint8_t* rdram, recomp_context* ctx) {
    g_list_pointer = ctx->r17;
    g_list_start = list_pointer(rdram);
    g_before_views.clear();
    let_go_of_memory();
    // Asked again here: leaving a session puts this machine's own game back after the frame's start,
    // and the match's view must not be drawn again over that game.
    g_view = local_view(rdram);
    // Paused, the game draws no views, only the copy of the last frame it made (the local view, drawn
    // whole) and the menu over the whole screen: that frame is shown as the game draws it.
    if (g_full && g_view >= 0 && MEM_BU(0, kseg0(kScreenHandler)) == 0 && MEM_BU(0, kseg0(kPaused)) == 0) {
        // Until the pass has put the memory back the renderer writes none of it (hold_game_memory).
        if (!g_holding) {
            conker::renderer::hold_game_memory();
            g_holding = true;
        }
        g_before_views.assign(rdram, rdram + kGameMemory);
        g_count_reads_before = conker::frame_input::count_reads();
    }
}

// func_1501878C at 0x15018CDC, the frame's list ended just before it is sent (func_1501C880): the
// views drawn again for the local player alone from the memory they started from, with the camera
// as the game's drawing left it, into memory the renderer draws this frame from (RDRAM as it is
// after the pass), and then the game's memory is put back as the game left it. While the pass runs
// no other thread runs and nothing from outside is delivered (ultramodern_isolated; a ROM read
// completes at once), so the game goes on exactly as on every other machine.
extern "C" void conker_my_view_draw_end(uint8_t* rdram, recomp_context* ctx) {
    if (g_drawing) {
        return;
    }
    if (g_before_views.empty()) {
        let_go_of_memory();
        return;
    }
    static std::vector<uint8_t> after;
    after.assign(rdram, rdram + kGameMemory);
    std::memcpy(rdram, g_before_views.data(), kGameMemory);
    // The local view's per-view entries as the game's drawing (its camera step) left them.
    auto keep = [&](uint32_t table, uint32_t size) {
        const uint32_t at = table + static_cast<uint32_t>(g_view) * size;
        if (in_rdram(table) && at + size <= 0x80000000u + kGameMemory) {
            std::memcpy(rdram + (at - 0x80000000u), after.data() + (at - 0x80000000u), size);
        }
    };
    keep(static_cast<uint32_t>(MEM_W(0, kseg0(kCameras))), kCameraSize);
    keep(static_cast<uint32_t>(MEM_W(0, kseg0(kViews))), kViewSize);
    keep(static_cast<uint32_t>(MEM_W(0, kseg0(kViewMatrices))), 0x40);
    keep(static_cast<uint32_t>(MEM_W(4, kseg0(kViewMatrices))), 0x40);
    keep(static_cast<uint32_t>(MEM_W(0, kseg0(kViewRecords))), 0xB0);
    keep(static_cast<uint32_t>(MEM_W(0, kseg0(kCameraCopies))), kCameraSize);
    const uint32_t count_reads = conker::frame_input::count_reads();
    conker::frame_input::set_count_reads(g_count_reads_before);
    g_drawing = true;
    recomp_dma_done_now = true;
    ultramodern_isolated = true;
    {
        StackGuard guard(rdram, ctx);
        draw_local_view(rdram, ctx, g_view);
    }
    ultramodern_isolated = false;
    recomp_dma_done_now = false;
    g_drawing = false;
    dump_pass(after.data(), rdram);
    conker::renderer::draw_next_frame_from(std::vector<uint8_t>(rdram, rdram + kImageSize));
    std::memcpy(rdram, after.data(), kGameMemory);
    conker::frame_input::set_count_reads(count_reads);
    let_go_of_memory();
}

bool conker::my_view::drawing_again() { return g_drawing; }

nlohmann::json conker::status::my_view() {
    return enhancement("my_view", "Online: only your own view and sounds", enabled(), "CONKER_MY_VIEW");
}

conker::switches::Switch conker::switches::my_view() {
    return {"MY VIEW", enabled, [](bool on) { g_enabled.store(on); }, true};
}
