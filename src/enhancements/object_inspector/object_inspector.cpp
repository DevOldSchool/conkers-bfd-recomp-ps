// The object inspector, a debug menu tool (INSPECT): point the mouse at something in the game and
// the overlay says what it is; a left click picks it out, highlighted in the picture and reported
// in the console in full, and a right click lets it go. CONKER_INSPECT=1 turns it on from the start.
//
// The game says what it draws. While the inspector is on, the functions that draw an actor, an
// attachment, a line of text or a part of the world (the [[patches.hook]] snippets in config/us.toml
// that call conker_inspect_enter) write a marker into the frame's display list as they start and
// another where they finish: G_SPNOOP commands RT64 reads as a draw tag to push and pop
// (tools/patches/rt64-object-picking.patch, RT64::SetDrawTagging). A hook runs the function itself
// in between, so the end is known: the function returns the display list's new end. The markers
// cost two commands a function and are only written while the list is short of its scene's limit
// (D_800BEBA4, 400 commands short of its real end), and a function whose return does not look like
// the list's end gets its start marker blanked rather than a stray end. Every draw call RT64 makes
// keeps the innermost tag and the display list its triangles came from.
//
// The renderer finds what is under the mouse (src/platform/object_picker_rt64.cpp) and highlights
// it and the picked thing; this file names it. CONKER_INSPECT_CURSOR=<x>,<y>[,<layer>][;...] (fractions
// of the window, and wheel steps) holds the mouse there instead and picks what is under it every two
// seconds, moving on to the next spot after each, so a scripted run reports them. Tags: an actor's index in the actor table, an
// attachment's parent actor and node, or the drawing function. Actor model numbers are bank 1 model
// entries, named from the decomp's model registry (model_names.inc, tools/gen_model_names.py).
#include "net/net_play.hpp"
#include "enhancements/object_inspector/object_inspector.hpp"
#include "net/lobby_ui.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <string>
#include <vector>

#include "recomp.h"

namespace conker::inspector {

namespace {

inline gpr kseg0(uint32_t address) { return static_cast<gpr>(static_cast<int32_t>(address)); }

constexpr uint32_t kActors = 0x800CC2D0;       // D_800CC2D0: the actor table
constexpr uint32_t kActorSize = 0x32C;
constexpr uint32_t kActorCount = 25;
constexpr uint32_t kProps = 0x800DBEF4;        // D_800DBEF4: the prop table (0xA0 bytes a prop)
constexpr uint32_t kPropSize = 0xA0;
constexpr uint32_t kScene = 0x800BE9F0;        // D_800BE9F0
constexpr uint32_t kListIndex = 0x800BE9C0;    // D_800BE9C0: which frame display list is built (u8)
constexpr uint32_t kLists = 0x800BE9C8;        // D_800BE9C8: the two frame display lists
constexpr uint32_t kListLimit = 0x800BEBA4;    // D_800BEBA4: commands the game lets itself write
constexpr uint32_t kListHeadroom = 400;        // the list's real end is this many commands further
constexpr uint32_t kTagPush = 0xE0D7A600;     // G_SPNOOP with RT64_DRAW_TAG_PUSH (rt64_render_hooks.h)
constexpr uint32_t kTagPop = 0xE0D7A601;      // and RT64_DRAW_TAG_POP
constexpr uint32_t kNoOp = 0xE0000000;

struct ModelName {
    int bank;
    int entry;
    int segment;
    const char* name;
};

constexpr ModelName kModelNames[] = {
#include "enhancements/object_inspector/model_names.inc"
};

struct FunctionName {
    uint32_t address;
    const char* name;
};

// The drawing functions config/us.toml tags, by what is known of them. The rest show as FUNC.
constexpr FunctionName kFunctionNames[] = {
    {0x1502BAD0, "ACTORS"},              // the actor table's draw loop
    {0x15030E08, "ATTACHMENTS"},         // the attachments' draw loop
    {0x150368C4, "CONKER EFFECT QUAD"},  // drawn after actor 0
    {0x1510B9D0, "LEVEL"},               // its view tests are the level geometry's (widescreen culling)
    {0x151135C4, "PROPS"},               // the prop table's draw loop
    {0x151674F8, "OBJECT LAYER"},        // draws a layer's objects with their kinds' callbacks
    {0x1517F3A0, "VIEWPORT FADE"},
};

std::atomic<bool> g_active{[] {
    const char* value = std::getenv("CONKER_INSPECT");
    return value != nullptr && value[0] == '1';
}()};

std::mutex g_mutex;
Cursor g_cursor;
int g_click = 0;
Hit g_hovered;
Hit g_picked;
uint32_t g_picked_serial = 0;
uint32_t g_reported_serial = 0;

// Game thread: the wrappers being run (conker_inspect_enter to conker_inspect_leave, or
// conker_inspect_push to conker_inspect_pop).
struct Wrapped {
    uint32_t function;
    uint32_t marker;  // where its start marker is, 0 if none was written
    uint32_t site;    // the call site of a push, 0 for a wrapped function
};
std::vector<Wrapped> g_wrapped;
std::vector<std::pair<uint32_t, uint32_t>> g_object_callbacks;  // object address, the callback that drew it last
std::vector<uint32_t> g_sprite_loads;  // game thread: the resources func_1510D0EC is loading
std::mutex g_sprites_mutex;
std::unordered_map<uint32_t, int> g_sprite_tiles;  // tile's physical address, its resource
uint32_t g_reentering = 0;  // the function a hook is about to run again
std::vector<uint32_t> g_warned;

const char* model_name(int bank, int entry) {
    for (const ModelName& model : kModelNames) {
        if (model.bank == bank && model.entry == entry && model.segment == 0) {
            return model.name;
        }
    }
    return nullptr;
}

// For the game's font: ASCII, upper case ("Conker — variant 1" -> "CONKER - VARIANT 1").
std::string game_text(const char* text) {
    std::string out;
    for (const unsigned char* c = reinterpret_cast<const unsigned char*>(text); *c != 0; c++) {
        if (*c < 0x80) {
            out += static_cast<char>(std::toupper(*c));
        } else if ((*c & 0xC0) != 0x80) {
            out += '-';  // the first byte of a multi-byte character (dashes, mostly)
        }
    }
    return out;
}

// The frame display list's start and the end the game may write to, if `address` is in it.
bool in_frame_list(uint8_t* rdram, uint32_t address, uint32_t* start, uint32_t* limit, uint32_t* end) {
    const uint32_t index = MEM_BU(0, kseg0(kListIndex)) & 1;
    const uint32_t list = static_cast<uint32_t>(MEM_W(index * 4, kseg0(kLists)));
    const uint32_t commands = static_cast<uint32_t>(MEM_W(0, kseg0(kListLimit)));
    if (list < 0x80000000 || commands == 0 || commands > 0x100000) {
        return false;
    }
    *start = list;
    *limit = list + commands * 8;
    *end = *limit + kListHeadroom * 8;
    return address >= list && address < *end && (address & 7) == 0;
}

void write_command(uint8_t* rdram, uint32_t address, uint32_t w0, uint32_t w1) {
    MEM_W(0, kseg0(address)) = static_cast<int32_t>(w0);
    MEM_W(4, kseg0(address)) = static_cast<int32_t>(w1);
}

uint32_t actor_index(uint32_t address) {
    const uint32_t offset = address - kActors;
    return offset < kActorSize * kActorCount ? offset / kActorSize : 0x1F;
}

uint32_t prop_index(uint8_t* rdram, uint32_t address) {
    const uint32_t offset = address - static_cast<uint32_t>(MEM_W(0, kseg0(kProps)));
    return offset % kPropSize == 0 && offset / kPropSize < 0x10000 ? offset / kPropSize : 0xFFFF;
}

uint32_t prop_address(uint8_t* rdram, uint32_t index) {
    return static_cast<uint32_t>(MEM_W(0, kseg0(kProps))) + index * kPropSize;
}

// The tag a wrapped function's drawing gets, from its arguments.
uint32_t tag_for(uint8_t* rdram, recomp_context* ctx, uint32_t function) {
    switch (function) {
        case 0x1502C974:  // func_1502C974(dl, actor index, view, mode, 0)
        case 0x1502C408:  // func_1502C408(dl, actor index)
            return make_tag(TagKind::Actor, static_cast<uint32_t>(ctx->r5) & 0xFF);
        case 0x150311C4:  // func_150311C4(dl, node, parent actor, view)
            return make_tag(TagKind::Attachment,
                            actor_index(static_cast<uint32_t>(ctx->r6)) << 23 | (static_cast<uint32_t>(ctx->r5) & 0x7FFFFF));
        case 0x150417AC:  // func_150417AC(dl, x, y, text, ...)
            return make_tag(TagKind::Text, 0);
        case 0x151137D4:  // func_151137D4(dl, prop, ...), from func_151135C4's loop
            return make_tag(TagKind::Prop, prop_index(rdram, static_cast<uint32_t>(ctx->r5)));
        default:
            return make_tag(TagKind::Function, function);
    }
}

std::string hex(uint64_t value, int digits) {
    char text[24];
    std::snprintf(text, sizeof(text), "%0*llX", digits, static_cast<unsigned long long>(value));
    return text;
}

bool actor_position(uint8_t* rdram, uint32_t index, float* x, float* y, float* z) {
    if (index >= kActorCount) {
        return false;
    }
    const gpr actor = kseg0(kActors + index * kActorSize);
    uint32_t bits[3] = {static_cast<uint32_t>(MEM_W(0x14, actor)), static_cast<uint32_t>(MEM_W(0x18, actor)),
                        static_cast<uint32_t>(MEM_W(0x1C, actor))};
    std::memcpy(x, &bits[0], 4);
    std::memcpy(y, &bits[1], 4);
    std::memcpy(z, &bits[2], 4);
    return true;
}

// "MODEL 108 SUNFLOWER" for an actor, with the name as the registry has it in `full`.
std::string actor_model(uint8_t* rdram, uint32_t index, std::string* full) {
    if (index >= kActorCount) {
        return "";
    }
    const int model = MEM_BU(4, kseg0(kActors + index * kActorSize));
    const char* name = model_name(1, model);
    if (full != nullptr) {
        *full = name != nullptr ? name : "";
    }
    return "MODEL " + std::to_string(model) + (name != nullptr ? " " + game_text(name) : "");
}

}  // namespace

// Not online: its markers go into the frame's display list, in game memory, on this machine only (and
// consistent controls read the pad another way while it is on), so a session it was left on for went
// out of sync. The debug menu that switches it is off there too.
bool active() { return g_active.load() && !conker::net::active() && !conker::net::in_place(); }

void set_active(bool on) {
    g_active = on;
    if (!on) {
        std::lock_guard lock(g_mutex);
        g_hovered = {};
        g_picked = {};
    }
}

void mouse_moved(float x, float y) {
    std::lock_guard lock(g_mutex);
    constexpr float kStill = 0.004f;  // of the window: moving less keeps the layer the wheel chose
    const bool still = g_cursor.inside && std::fabs(x - g_cursor.x) < kStill && std::fabs(y - g_cursor.y) < kStill;
    g_cursor = {true, still ? g_cursor.x : x, still ? g_cursor.y : y, still ? g_cursor.layer : 0};
}

void mouse_wheel(int steps) {
    std::lock_guard lock(g_mutex);
    g_cursor.layer = std::max(0, g_cursor.layer + steps);
}

void mouse_left() {
    std::lock_guard lock(g_mutex);
    g_cursor.inside = false;
}

void mouse_clicked(int button) {
    if (!active()) {
        return;
    }
    std::lock_guard lock(g_mutex);
    g_click = button;
}

// CONKER_INSPECT_CURSOR's spots, if set.
const std::vector<Cursor>& fixed_spots() {
    static const std::vector<Cursor> spots = [] {
        std::vector<Cursor> list;
        for (const char* p = std::getenv("CONKER_INSPECT_CURSOR"); p != nullptr && *p != '\0';) {
            Cursor c;
            c.inside = std::sscanf(p, "%f,%f,%d", &c.x, &c.y, &c.layer) >= 2;
            if (c.inside) {
                list.push_back(c);
            }
            p = std::strchr(p, ';');
            p = p != nullptr ? p + 1 : nullptr;
        }
        return list;
    }();
    return spots;
}

std::atomic<size_t> g_fixed_spot{0};

bool fixed_cursor(Cursor* cursor) {
    const std::vector<Cursor>& spots = fixed_spots();
    if (spots.empty()) {
        return false;
    }
    *cursor = spots[g_fixed_spot.load() % spots.size()];
    return true;
}

Cursor cursor() {
    Cursor fixed;
    if (fixed_cursor(&fixed)) {
        return fixed;
    }
    std::lock_guard lock(g_mutex);
    return g_cursor;
}

int take_click() {
    Cursor fixed;
    static int frames = 0;
    if (fixed_cursor(&fixed) && ++frames % 60 == 0) {
        g_fixed_spot++;  // after this frame's pick
        return 1;
    }
    std::lock_guard lock(g_mutex);
    const int click = g_click;
    g_click = 0;
    return click;
}

void publish(const Hit& hovered, const Hit& picked, uint32_t picked_serial) {
    std::lock_guard lock(g_mutex);
    g_hovered = hovered;
    g_picked = picked;
    g_picked_serial = picked_serial;
}

void latest(Hit* hovered, Hit* picked, uint32_t* picked_serial) {
    std::lock_guard lock(g_mutex);
    *hovered = g_hovered;
    *picked = g_picked;
    *picked_serial = g_picked_serial;
}

int sprite_resource(uint32_t address) {
    std::lock_guard lock(g_sprites_mutex);
    const auto found = g_sprite_tiles.find(address & 0x1FFFFFFF);
    return found != g_sprite_tiles.end() ? found->second : -1;
}

bool same_thing(const Hit& hit, uint32_t tag, uint32_t list, uint64_t texture) {
    if (!hit.valid || hit.tag != tag) {
        return false;
    }
    const TagKind kind = tag_kind(tag);
    return kind == TagKind::Actor || kind == TagKind::Attachment || kind == TagKind::Prop ||
           (hit.list == list && hit.texture == texture);
}

std::string describe_tag(uint8_t* rdram, uint32_t tag) {
    const uint32_t payload = tag_payload(tag);
    switch (tag_kind(tag)) {
        case TagKind::Actor:
            return "ACTOR " + std::to_string(payload) + " " + actor_model(rdram, payload, nullptr);
        case TagKind::Attachment: {
            const uint32_t parent = payload >> 23;
            return "ATTACHMENT " + hex(0x80000000 | (payload & 0x7FFFFF), 8) + " ON ACTOR " + std::to_string(parent);
        }
        case TagKind::Function: {
            const uint32_t function = 0x10000000 | payload;
            for (const FunctionName& known : kFunctionNames) {
                if (known.address == function) {
                    return std::string(known.name) + " (" + hex(function, 8) + ")";
                }
            }
            return "FUNC " + hex(function, 8);
        }
        case TagKind::Text:
            return "TEXT";
        case TagKind::Prop:
            return "PROP " + std::to_string(payload);
        case TagKind::Object: {
            const uint32_t object = 0x80000000 | payload;
            std::string text = "OBJECT " + hex(object, 8);
            for (const auto& [address, callback] : g_object_callbacks) {
                if (address == object) {
                    text += " BY " + hex(callback, 8);
                }
            }
            return text;
        }
        default:
            return "UNTAGGED";
    }
}

std::string describe(uint8_t* rdram, const Hit& hit) {
    if (hit.rect && hit.sprite[0] != 0) {
        std::string name = "SPRITE ";
        for (const char* c = hit.sprite; *c != 0; c++) {
            name += static_cast<char>(std::toupper(static_cast<unsigned char>(*c)));
        }
        return tag_kind(hit.tag) == TagKind::None ? name : name + " IN " + describe_tag(rdram, hit.tag);
    }
    if (hit.rect && tag_kind(hit.tag) == TagKind::None) {
        return "2D SPRITE";
    }
    return describe_tag(rdram, hit.tag);
}

std::string overlay_line(uint8_t* rdram, int line) {
    Hit hovered, picked;
    uint32_t serial;
    latest(&hovered, &picked, &serial);
    switch (line) {
        case 0:
            if (!hovered.valid) {
                return "INSPECT: POINT AT SOMETHING, CLICK TO PICK";
            }
            return (hovered.layers > 1 ? "> " + std::to_string(hovered.layer + 1) + "/" + std::to_string(hovered.layers) + " "
                                       : std::string("> ")) +
                   describe(rdram, hovered);
        case 1:
            if (!hovered.valid) {
                return "";
            }
            return "DL " + hex(0x80000000 | hovered.list, 8) +
                   (hovered.rect ? std::string("  2D") : "  TRIS " + std::to_string(hovered.triangles)) +
                   (hovered.texture != 0 ? "  TEX " + hex(hovered.texture, 16) : "");
        case 2: {
            float x, y, z;
            char text[64];
            if (hovered.valid && tag_kind(hovered.tag) == TagKind::Actor &&
                actor_position(rdram, tag_payload(hovered.tag), &x, &y, &z)) {
                std::snprintf(text, sizeof(text), "AT %.0f %.0f %.0f", x, y, z);
                return text;
            }
            if (hovered.valid && tag_kind(hovered.tag) == TagKind::Prop) {
                const gpr prop = kseg0(prop_address(rdram, tag_payload(hovered.tag)));
                std::snprintf(text, sizeof(text), "AT %d %d %d", MEM_H(0x10, prop), MEM_H(0x12, prop), MEM_H(0x14, prop));
                return text;
            }
            return "";
        }
        case 3:
            return picked.valid ? "PICKED " + describe(rdram, picked) : "";
        default:
            return "";
    }
}

void report_pick(uint8_t* rdram) {
    Hit hovered, picked;
    uint32_t serial;
    latest(&hovered, &picked, &serial);
    if (serial == g_reported_serial) {
        return;
    }
    g_reported_serial = serial;
    if (!picked.valid) {
        std::printf("[conker] inspect: nothing picked\n");
        return;
    }
    const uint32_t payload = tag_payload(picked.tag);
    std::printf("[conker] inspect: picked %s\n", describe(rdram, picked).c_str());
    std::printf("    scene %d, tag %08X, frame buffer pixel %.1f, %.1f\n", MEM_W(0, kseg0(kScene)), picked.tag,
                picked.screen_x, picked.screen_y);
    if (picked.rect) {
        std::printf("    draw call %u: a 2D rectangle from display list %08X%s%s\n", picked.call, 0x80000000 | picked.list,
                    picked.sprite[0] != 0 ? ", sprite " : "", picked.sprite);
    } else {
        std::printf("    draw call %u: %u triangles from display list %08X, the first drawn by the command at %08X "
                    "with vertices at %08X\n",
                    picked.call, picked.triangles, 0x80000000 | picked.list, 0x80000000 | picked.command,
                    0x80000000 | picked.vertices);
    }
    std::printf("    combiner %08X %08X, other mode %08X %08X; %u draw calls make up the picked thing\n",
                picked.combiner[0], picked.combiner[1], picked.other_mode[0], picked.other_mode[1], picked.calls);
    if (picked.texture != 0) {
        std::printf("    texture hash %016llX\n", static_cast<unsigned long long>(picked.texture));
    }
    if (tag_kind(picked.tag) == TagKind::Actor) {
        const gpr actor = kseg0(kActors + payload * kActorSize);
        std::string name;
        actor_model(rdram, payload, &name);
        float x, y, z;
        actor_position(rdram, payload, &x, &y, &z);
        std::printf("    actor %u at %08X: model %d (bank 1 entry%s%s), kind %d, position %.1f %.1f %.1f\n", payload,
                    kActors + payload * kActorSize, MEM_BU(4, actor), name.empty() ? "" : ", ", name.c_str(),
                    MEM_BU(5, actor), x, y, z);
    } else if (tag_kind(picked.tag) == TagKind::Attachment) {
        const uint32_t node = 0x80000000 | (payload & 0x7FFFFF);
        std::printf("    attachment node %08X:", node);
        for (int i = 0; i < 16; i++) {
            std::printf(" %02X", MEM_BU(i, kseg0(node)));
        }
        std::printf(" ...; parent actor %u\n", payload >> 23);
    } else if (tag_kind(picked.tag) == TagKind::Prop) {
        const uint32_t address = prop_address(rdram, payload);
        const gpr prop = kseg0(address);
        std::printf("    prop %u at %08X: position %d %d %d, model data %08X; record:\n", payload, address,
                    MEM_H(0x10, prop), MEM_H(0x12, prop), MEM_H(0x14, prop), static_cast<uint32_t>(MEM_W(0x1C, prop)));
        for (uint32_t row = 0; row < kPropSize; row += 16) {
            std::printf("      +%02X:", row);
            for (uint32_t i = row; i < row + 16; i++) {
                std::printf(" %02X", MEM_BU(static_cast<int>(i), prop));
            }
            std::printf("\n");
        }
    }
}

}  // namespace conker::inspector

using namespace conker::inspector;

// At the start of a drawing function config/us.toml tags (`function`, its address; $a0 the display
// list, returned in $v0): returns 1 when the hook should run the function itself and then
// conker_inspect_leave, having written the start marker, and 0 when it should go on as usual (the
// inspector is off, or this is that run).
namespace {

// The start marker for `tag` at $a0, which moves past it.
void begin(uint8_t* rdram, recomp_context* ctx, uint32_t function, uint32_t site, uint32_t tag) {
    const uint32_t list = static_cast<uint32_t>(ctx->r4);
    uint32_t start, limit, end;
    Wrapped wrapped{function, 0, site};
    if (in_frame_list(rdram, list, &start, &limit, &end) && list + 8 <= limit) {
        write_command(rdram, list, kTagPush, tag);
        wrapped.marker = list;
        ctx->r4 = kseg0(list + 8);
    }
    g_wrapped.push_back(wrapped);
}

// The end marker at $v0, the end of the list the function returned, which moves past it.
void end(uint8_t* rdram, recomp_context* ctx, uint32_t site) {
    if (g_wrapped.empty() || g_wrapped.back().site != site) {
        return;
    }
    const Wrapped wrapped = g_wrapped.back();
    g_wrapped.pop_back();
    if (wrapped.marker == 0) {
        return;
    }
    const uint32_t returned = static_cast<uint32_t>(ctx->r2);
    uint32_t start, limit, end;
    if (in_frame_list(rdram, returned, &start, &limit, &end) && returned >= wrapped.marker + 8 && returned + 8 <= end) {
        write_command(rdram, returned, kTagPop, 0);
        ctx->r2 = kseg0(returned + 8);
        return;
    }
    write_command(rdram, wrapped.marker, kNoOp, 0);
    bool warned = false;
    for (uint32_t function : g_warned) {
        warned |= function == wrapped.function;
    }
    if (!warned) {
        g_warned.push_back(wrapped.function);
        std::printf("[conker] inspect: func_%08X returned %08X, not its display list's end; left untagged\n",
                    wrapped.function, returned);
    }
}

}  // namespace

extern "C" int conker_inspect_enter(uint8_t* rdram, recomp_context* ctx, uint32_t function) {
    if (g_reentering == function) {
        g_reentering = 0;
        return 0;
    }
    if (!active()) {
        return 0;
    }
    begin(rdram, ctx, function, 0, tag_for(rdram, ctx, function));
    g_reentering = function;
    return 1;
}

// After the hook ran the function: the end marker, at the end it returned.
extern "C" void conker_inspect_leave(uint8_t* rdram, recomp_context* ctx) { end(rdram, ctx, 0); }

// Before func_151674F8 calls a callback through a register (`callback`, $a0 the display list): a start
// marker naming the object it draws ($a1, `object` set) or else the callback. `site` is the call's
// address, which conker_inspect_pop is given after the call.
extern "C" void conker_inspect_push(uint8_t* rdram, recomp_context* ctx, uint32_t site, uint32_t callback, int object) {
    if (!active()) {
        return;
    }
    uint32_t tag = make_tag(TagKind::Function, callback);
    if (object != 0) {
        const uint32_t address = static_cast<uint32_t>(ctx->r5);
        tag = make_tag(TagKind::Object, address & 0x7FFFFF);
        bool known = false;
        for (auto& [known_address, known_callback] : g_object_callbacks) {
            if (known_address == address) {
                known_callback = callback;
                known = true;
            }
        }
        if (!known && g_object_callbacks.size() < 4096) {
            g_object_callbacks.emplace_back(address, callback);
        }
    }
    begin(rdram, ctx, callback, site, tag);
}

extern "C" void conker_inspect_pop(uint8_t* rdram, recomp_context* ctx, uint32_t site) { end(rdram, ctx, site); }

// At the start of func_1510D0EC ($a0 the sprite resource), always: returns 1 when the hook should run
// it itself and then conker_inspect_sprite_loaded, 0 when this is that run.
extern "C" int conker_inspect_sprite_enter(uint8_t* /*rdram*/, recomp_context* /*ctx*/, uint32_t resource) {
    if (g_reentering == 0x1510D0EC) {
        g_reentering = 0;
        return 0;
    }
    g_sprite_loads.push_back(resource);
    g_reentering = 0x1510D0EC;
    return 1;
}

// After it: $v0 is the resource's tile, or 0x80000000 while it is not loaded yet.
extern "C" void conker_inspect_sprite_loaded(uint8_t* /*rdram*/, recomp_context* ctx) {
    if (g_sprite_loads.empty()) {
        return;
    }
    const uint32_t resource = g_sprite_loads.back();
    g_sprite_loads.pop_back();
    const uint32_t tile = static_cast<uint32_t>(ctx->r2);
    if (tile != 0x80000000 && tile != 0) {
        std::lock_guard lock(g_sprites_mutex);
        g_sprite_tiles[tile & 0x1FFFFFFF] = static_cast<int>(resource);
    }
    // The ONLINE lobby's page on the MULTI barrel hides the barrel's own pictures: "not loaded yet",
    // which every caller skips.
    if (conker::lobby_ui::hides_sprite(resource)) {
        ctx->r2 = static_cast<gpr>(static_cast<int32_t>(0x80000000u));
    }
}
