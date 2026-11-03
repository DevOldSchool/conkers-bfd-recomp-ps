// The game shows the N64's buttons wherever it asks for a press. The menus have A and B, each in a
// dark and a lit frame, START dark and lit (the multiplayer join screen), and small C-left and
// C-right; the tutorial bubbles have A and B and their own Z, L, R and C buttons (the sprites
// func_150417AC picks). They are RGBA32 and RGBA16 tiles in the ROM's flat resources
// (src/enhancements/sharp_menus/menu_art.cpp). With an Xbox or PlayStation pad connected, they are
// replaced with that pad's buttons, and with none (the keyboard alone) with the keys that press
// them, drawn like Rare's when the set is first wanted
// (src/enhancements/pad_icons/button_art.cpp), through a texture pack keyed by the hash RT64
// computes for each tile as the game loads it (as src/enhancements/sharp_menus does).
// CONKER_DUMP_PAD_ICONS=<dir> writes the pack out.
//
// Other pads get the N64's buttons redrawn the same way, sharper than Rare's
// small pictures, unless the OPTIONS menu asks for the original text and menus.
//
// The N64 buttons map to the pad's by position by default (src/platform/input_sdl.cpp): A to the
// bottom face button (Xbox A, Cross), B to the right and left ones (Xbox B and X, drawn as B; Circle
// and Square, drawn as Square), START to Menu or Options, Z to either trigger (drawn as the right
// one), L and R to the shoulder buttons and the C buttons to the right stick. The stick is swapped
// left to right (C-left is pushing it right), so the C-left picture shows the stick pushed right,
// and C-right pushed left. The debug menu's CONTROLS page maps them differently: an Xbox or
// PlayStation set then shows what controller 1's mapping has press each button (any of the pad's
// controls can be drawn), and a new set is drawn when it changes. The B pads on the ground show a
// PlayStation Square only while B is still Square, and are Rare's B otherwise. The keys:
// A Space, B E, START Enter, Z C (crouch; Left Ctrl and the left mouse button too), L Q, R F, and
// the C buttons I, K and J and L, swapped as the stick is.
#include "enhancements/pad_icons/pad_icons.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "hle/rt64_application.h"
#include "common/rt64_load_types.h"
#include "common/rt64_tmem_hasher.h"
#include "enhancements/pad_icons/b_pad.hpp"
#include "enhancements/pad_icons/button_art.hpp"
#include "enhancements/sharp_menus/menu_art.hpp"
#include "platform/input.hpp"
#include "platform/memory_pack.hpp"

namespace conker::pad_icons {

namespace {

struct Button {
    int resource;
    int width, height;  // texels
    int bits;           // per texel: RGBA32 or RGBA16
    const char* picture;
};

constexpr Button kButtons[] = {
    // The menus
    {2164, 32, 32, 32, "a"}, {2165, 32, 32, 32, "a_lit"}, {2168, 32, 32, 32, "b"}, {2169, 32, 32, 32, "b_lit"},
    {2206, 32, 32, 32, "start"}, {2207, 32, 32, 32, "start_lit"},
    {2224, 16, 16, 32, "c_left_small"}, {2225, 16, 16, 32, "c_right_small"},
    // The tutorial bubbles (A and B are the menus' dark ones)
    {7731, 16, 32, 16, "z"}, {2941, 32, 16, 16, "l"}, {3668, 32, 16, 16, "r"},
    {2985, 32, 32, 16, "c_left"}, {3744, 32, 32, 16, "c_right"}, {4695, 32, 32, 16, "c_up"}, {1689, 32, 32, 16, "c_down"},
};

// RT64's hash for a tile loaded as the game loads them: LoadBlock to TMEM 0, the stored bytes as
// they are for RGBA16; for RGBA32 each texel's red and green go to the lower half of TMEM and its
// blue and alpha to the upper half.
uint64_t hash(const std::vector<uint8_t>& tile, const Button& button) {
    std::vector<uint8_t> tmem(4096, 0);
    const int texels = button.width * button.height;
    if (button.bits == 32) {
        for (int i = 0; i < texels; i++) {
            tmem[i * 2] = tile[i * 4], tmem[i * 2 + 1] = tile[i * 4 + 1];
            tmem[2048 + i * 2] = tile[i * 4 + 2], tmem[2048 + i * 2 + 1] = tile[i * 4 + 3];
        }
    } else {
        std::copy_n(tile.begin(), texels * 2, tmem.begin());
    }
    RT64::LoadTile load{};
    load.fmt = 0;                                     // G_IM_FMT_RGBA
    load.siz = button.bits == 32 ? 3 : 2;             // G_IM_SIZ_32b, G_IM_SIZ_16b
    load.line = uint16_t(button.width * 2 / 8);       // 16-bit halves of a row, in 64-bit words
    return RT64::TMEMHasher::hash(tmem.data(), load, button.width, button.height, 0, RT64::TMEMHasher::CurrentHashVersion);
}

} // namespace

Shown shown(Style style, bool camera_swapped) {
    Shown out;
    out.fill(0);
    if (style != Style::Xbox && style != Style::PlayStation && style != Style::Keyboard) return out;
    const input::PadKind kind = style == Style::PlayStation ? input::PadKind::PlayStation : input::PadKind::Xbox;
    // The mapping's buttons are its rows: C-LEFT is the N64's C-right, which the camera swap
    // makes C-left (input.hpp's kMappedButtons).
    auto row = [](uint16_t n64) {
        for (int i = 0; i < input::kMappedButtonCount; i++) {
            if (input::kMappedButtons[i].n64 == n64) return i;
        }
        return -1;
    };
    const uint16_t c_left = camera_swapped ? input::kCLeft : input::kCRight;
    const uint16_t c_right = camera_swapped ? input::kCRight : input::kCLeft;
    const uint16_t buttons[kSlots] = {input::kA, input::kB, input::kStart, input::kZ, input::kL, input::kR,
                                      c_left, c_right, input::kCUp, input::kCDown};
    for (int slot = 0; slot < kSlots; slot++) {
        out[slot] = style == Style::Keyboard ? input::shown_key(row(buttons[slot]))
                                             : int(input::shown_control(0, row(buttons[slot]), kind));
    }
    return out;
}

std::shared_ptr<RT64::FileSystem> make_pack(Style style, const Shown& shown) {
    if (style == Style::Original) return nullptr;
    const std::string folder = style == Style::Xbox          ? "xbox"
                               : style == Style::PlayStation ? "playstation"
                               : style == Style::Keyboard    ? "keyboard"
                                                             : "n64";
    const auto started = std::chrono::steady_clock::now();
    const std::vector<Picture> pictures = draw_set(style, shown);
    auto pack = std::make_shared<memory_pack::Pack>();
    for (const Button& button : kButtons) {
        const std::vector<uint8_t> tile = menu_art::load_resource(button.resource);
        const auto picture = std::find_if(pictures.begin(), pictures.end(), [&](const Picture& p) { return p.name == button.picture; });
        if (picture == pictures.end() && style != Style::N64) {
            continue;  // a button nothing on the pad presses keeps Rare's picture
        }
        if (tile.size() != size_t(button.width) * button.height * button.bits / 8 || picture == pictures.end()) {
            std::fprintf(stderr, "[conker] pad buttons: no %s picture for resource %d; left as it is\n", button.picture, button.resource);
            continue;
        }
        pack->add("pad_icons/" + folder + "/" + button.picture + ".png", hash(tile, button), picture->width, picture->height,
                  picture->rgba.data());
    }
    // The B pads on the ground: an Xbox pad's B is B, so only a PlayStation pad's (a Square) and the
    // keyboard's (an E) change.
    // Only while B is still Square or E: the pads have no other mark.
    const bool b_is_square = shown[kSlotB] == int(input::PadControl::West);
    const bool b_is_e = shown[kSlotB] != 0 && input::key_label(shown[kSlotB]) == "E";
    if ((style == Style::PlayStation && b_is_square) || (style == Style::Keyboard && b_is_e)) {
        const Mark::Shape shape = style == Style::PlayStation ? Mark::Shape::Square : Mark::Shape::E;
        const std::string pads = "pad_icons/" + folder + "/";
        if (const std::optional<Texture> pad = wood_pad(shape)) {
            pack->add(pads + "wood_pad.png", pad->hash, pad->width, pad->height, pad->rgba.data());
        } else {
            std::fprintf(stderr, "[conker] pad buttons: no B pad texture in the ROM; left as it is\n");
        }
        const std::vector<Texture> stone = stone_pad(shape);
        for (size_t i = 0; i < stone.size(); i++) {
            pack->add(pads + "stone_pad_" + std::to_string(i) + ".png", stone[i].hash, stone[i].width,
                      stone[i].height, stone[i].rgba.data());
        }
        if (stone.empty()) std::fprintf(stderr, "[conker] pad buttons: no stone B pad textures in the ROM; left as they are\n");
        const std::vector<Texture> hex = hex_pad(shape);
        for (size_t i = 0; i < hex.size(); i++) {
            pack->add(pads + "hex_pad_" + std::to_string(i) + ".png", hex[i].hash, hex[i].width,
                      hex[i].height, hex[i].rgba.data());
        }
        if (hex.empty()) std::fprintf(stderr, "[conker] pad buttons: no hexagonal B pad textures in the ROM; left as they are\n");
        if (const std::optional<Texture> square = square_pad(shape)) {
            pack->add(pads + "square_pad.png", square->hash, square->width, square->height,
                      square->rgba.data());
        } else {
            std::fprintf(stderr, "[conker] pad buttons: no square B pad texture in the ROM; left as it is\n");
        }
        const std::vector<Texture> oval = oval_pad(shape);
        for (size_t i = 0; i < oval.size(); i++) {
            pack->add(pads + "oval_pad_" + std::to_string(i) + ".png", oval[i].hash, oval[i].width,
                      oval[i].height, oval[i].rgba.data());
        }
        if (oval.empty()) std::fprintf(stderr, "[conker] pad buttons: no teal B pad textures in the ROM; left as they are\n");
        if (const std::optional<Texture> rock = rock_pad(shape)) {
            pack->add(pads + "rock_pad.png", rock->hash, rock->width, rock->height, rock->rgba.data());
        } else {
            std::fprintf(stderr, "[conker] pad buttons: no grey stone B pad texture in the ROM; left as it is\n");
        }
        if (const std::optional<Texture> rug = rug_pad(shape)) {
            pack->add(pads + "rug_pad.png", rug->hash, rug->width, rug->height, rug->rgba.data());
        } else {
            std::fprintf(stderr, "[conker] pad buttons: no rug B pad texture in the ROM; left as it is\n");
        }
        const std::vector<Texture> plate = plate_pad(shape);
        for (size_t i = 0; i < plate.size(); i++) {
            pack->add(pads + "plate_pad_" + std::to_string(i) + ".png", plate[i].hash, plate[i].width,
                      plate[i].height, plate[i].rgba.data());
        }
        if (plate.empty()) std::fprintf(stderr, "[conker] pad buttons: no round stone B plate textures in the ROM; left as they are\n");
        const std::vector<Texture> crate = crate_pad(shape);
        for (size_t i = 0; i < crate.size(); i++) {
            pack->add(pads + "crate_pad_" + std::to_string(i) + ".png", crate[i].hash, crate[i].width,
                      crate[i].height, crate[i].rgba.data());
        }
        if (crate.empty()) std::fprintf(stderr, "[conker] pad buttons: no crate-top B pad textures in the ROM; left as they are\n");
        if (const std::optional<Texture> block = block_pad(shape)) {
            pack->add(pads + "block_pad.png", block->hash, block->width, block->height, block->rgba.data());
        } else {
            std::fprintf(stderr, "[conker] pad buttons: no stone block B pad texture in the ROM; left as it is\n");
        }
        const std::vector<Texture> disc = disc_pad(shape);
        for (size_t i = 0; i < disc.size(); i++) {
            pack->add(pads + "disc_pad_" + std::to_string(i) + ".png", disc[i].hash, disc[i].width,
                      disc[i].height, disc[i].rgba.data());
        }
        if (disc.empty()) std::fprintf(stderr, "[conker] pad buttons: no purple disc B pad textures in the ROM; left as they are\n");
        const std::vector<Texture> rust = rust_pad(shape);
        for (size_t i = 0; i < rust.size(); i++) {
            pack->add(pads + "rust_pad_" + std::to_string(i) + ".png", rust[i].hash, rust[i].width,
                      rust[i].height, rust[i].rgba.data());
        }
        if (rust.empty()) std::fprintf(stderr, "[conker] pad buttons: no rusty B plate textures in the ROM; left as they are\n");
        if (const std::optional<Texture> riveted = riveted_pad(shape)) {
            pack->add(pads + "riveted_pad.png", riveted->hash, riveted->width, riveted->height, riveted->rgba.data());
        } else {
            std::fprintf(stderr, "[conker] pad buttons: no riveted B plate texture in the ROM; left as it is\n");
        }
        if (const std::optional<Texture> panel = panel_pad(shape)) {
            pack->add(pads + "panel_pad.png", panel->hash, panel->width, panel->height, panel->rgba.data());
        } else {
            std::fprintf(stderr, "[conker] pad buttons: no dark stone panel B pad texture in the ROM; left as it is\n");
        }
        const std::vector<Texture> tile = tile_pad(shape);
        for (size_t i = 0; i < tile.size(); i++) {
            pack->add(pads + "tile_pad_" + std::to_string(i) + ".png", tile[i].hash, tile[i].width,
                      tile[i].height, tile[i].rgba.data());
        }
        if (tile.empty()) std::fprintf(stderr, "[conker] pad buttons: no red wooden B tile textures in the ROM; left as they are\n");
    }
    if (pack->files.empty()) return nullptr;
    pack->finish();
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    std::printf("[conker] %s buttons drawn in %lld ms\n", name(style), static_cast<long long>(took.count()));
    if (const char* dir = std::getenv("CONKER_DUMP_PAD_ICONS"); dir != nullptr && dir[0] != '\0') {
        memory_pack::write_files(*pack, dir);
    }
    return pack;
}

} // namespace conker::pad_icons
