// The object inspector, a debug menu tool: point the mouse at something in the game to see what
// it is, click to pick it out (see object_inspector.cpp).
#pragma once

#include <cstdint>
#include <string>

namespace conker::inspector {

// What the game was drawing, as its draw tags (object_inspector.cpp) name it: a kind in the top
// four bits and what it says in the rest.
enum class TagKind : uint32_t {
    None = 0,        // drawn outside every tagged function
    Actor = 1,       // index in the actor table D_800CC2D0
    Attachment = 2,  // parent actor's index << 23 | the attachment node's RDRAM offset
    Function = 3,    // the drawing function's address & 0x0FFFFFFF (the top nibble is 1)
    Text = 4,        // a line of text (func_150417AC); never picked
    Prop = 5,        // index in the prop table *D_800DBEF4
    Object = 6,      // an object func_151674F8 draws with its kind's callback: the object's RDRAM offset
};

constexpr TagKind tag_kind(uint32_t tag) { return static_cast<TagKind>(tag >> 28); }
constexpr uint32_t tag_payload(uint32_t tag) { return tag & 0x0FFFFFFF; }
constexpr uint32_t make_tag(TagKind kind, uint32_t payload) {
    return static_cast<uint32_t>(kind) << 28 | (payload & 0x0FFFFFFF);
}

// Whether the inspector is on (debug menu INSPECT, or CONKER_INSPECT=1 at start). While it is,
// the game marks what it draws and the renderer finds what is under the mouse.
bool active();
void set_active(bool on);

// The mouse, from the window's events (main thread): its place as a fraction of the window's
// size, and the buttons clicked.
void mouse_moved(float x, float y);
void mouse_left();
void mouse_clicked(int button);  // 1 left: pick out what is under it; 3 right: let it go
void mouse_wheel(int steps);     // positive: look one thing further behind what is in front

struct Cursor {
    bool inside = false;
    float x = 0.0f;  // fractions of the window's width and height
    float y = 0.0f;
    int layer = 0;   // things under it skipped, front to back (the wheel; 0 again once it moves)
};
Cursor cursor();

// For the renderer: the click waiting to be applied, if any (1 pick, 3 let go, 0 none).
int take_click();

// A draw call the renderer found under the mouse, or picked out.
struct Hit {
    bool valid = false;
    uint32_t tag = 0;
    uint32_t list = 0;        // RDRAM address of the display list the triangles came from,
    uint32_t command = 0;     // of the command that drew the first of them (0 for a rectangle)
    uint32_t vertices = 0;    // and of the vertices it was given
    uint32_t triangles = 0;   // in the draw call
    uint32_t calls = 0;       // draw calls that make up the same thing
    uint32_t call = 0;        // the draw call's index in the frame
    uint64_t texture = 0;     // RT64's hash of its first texture (0 if none)
    uint32_t combiner[2] = {0, 0};
    uint32_t other_mode[2] = {0, 0};
    bool rect = false;        // a 2D rectangle rather than triangles
    float screen_x = 0.0f;    // where the mouse is, in the game's frame buffer pixels
    float screen_y = 0.0f;
    int layer = 0;            // its place among the things under the mouse, front to back,
    int layers = 0;           // and how many there are
    uint32_t texture_address = 0;  // RDRAM address its first texture was loaded from
    char sprite[48] = {};     // for a rectangle drawing a sprite tile, which ("GAME1 2/3 (2012)", "2034")
};

// The sprite resource whose tile func_1510D0EC loaded at RDRAM `address` (any segment), or -1.
// Safe to call from any thread.
int sprite_resource(uint32_t address);

// The renderer's findings for the last frame (gfx thread), and the picked-out thing (or none).
// `picked_serial` changes whenever a new one is picked out.
void publish(const Hit& hovered, const Hit& picked, uint32_t picked_serial);
void latest(Hit* hovered, Hit* picked, uint32_t* picked_serial);

// Whether a draw call (its tag, display list and first texture's hash) is part of the same thing as
// a hit: the same actor or attachment, or else drawn by the same function from the same display list
// with the same texture (the level is one display list, so a piece of it is its material). The
// renderer highlights every draw call that matches.
bool same_thing(const Hit& hit, uint32_t tag, uint32_t list, uint64_t texture);

// What a hit is, for the overlay: "ACTOR 1 SUNFLOWER" (ASCII, upper case; reads the actor table).
std::string describe(uint8_t* rdram, const Hit& hit);

// The overlay's lines while the inspector is on (debug menu, game thread), and the report of a newly
// picked-out thing printed to the console.
std::string overlay_line(uint8_t* rdram, int line);
void report_pick(uint8_t* rdram);

} // namespace conker::inspector
