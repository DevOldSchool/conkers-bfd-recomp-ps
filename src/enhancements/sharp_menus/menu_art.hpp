// The menus' lettered artwork as the game stores it in the ROM, and the image operations the sharper
// menus are made with (src/enhancements/sharp_menus/sharp_menus.cpp, pause_signs.cpp).
#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace conker::menu_art {

// An RGBA image with premultiplied alpha, each channel 0..255.
struct Image {
    int width = 0;
    int height = 0;
    std::vector<float> pixels;  // width x height x 4, rows top first

    Image() = default;
    Image(int width, int height) : width(width), height(height), pixels(size_t(width) * height * 4, 0.0f) {}
    float* at(int x, int y) { return &pixels[(size_t(y) * width + x) * 4]; }
    const float* at(int x, int y) const { return &pixels[(size_t(y) * width + x) * 4]; }
};

// A 32x32 RGBA32 tile as stored: rows top first, odd rows with the two 8-byte halves of each 16-byte
// group exchanged, as the game's LoadBlock leaves them in TMEM.
using StoredTile = std::vector<uint8_t>;  // 4096 bytes

// Flat resource `id` from the ROM's indexed asset area, inflated; empty if it is missing.
std::vector<uint8_t> load_resource(int id);

// Resource `id` as a 32x32 RGBA32 tile, or nothing if it is not one.
std::optional<StoredTile> load_tile(int id);

// Tiles `first` .. `first + count - 1` side by side, as one image.
std::optional<Image> load_row(int first, int count);

Image decode_tile(const StoredTile& tile);
StoredTile encode_tile(const Image& image, int x0);  // the 32x32 at column x0, rounded to bytes

// Straight (not premultiplied) RGBA bytes, rounded.
std::vector<uint8_t> to_rgba8(const Image& image);

// Super-xBR (Hyllian's edge-directed 2x pixel-art upscaler, applied until `scale`, a power of two).
// Edges are found on luma and alpha; every channel is interpolated on premultiplied alpha and
// clamped to the four pixels around it, so edges do not ring. The border repeats, as the game's
// clamped tiles do. Each original pixel stays at the top left of its block.
Image super_xbr(const Image& image, int scale);

// Turned `degrees` counter-clockwise about its centre with bicubic filtering, on a canvas grown
// to hold all of it.
Image rotate(const Image& image, double degrees);

// The tilt of the opaque part, in degrees counter-clockwise, from its principal axis.
double tilt(const Image& image);

// The smallest box holding every pixel with alpha above `alpha`: [x0, x1) x [y0, y1).
struct Box {
    int x0 = 0, x1 = 0, y0 = 0, y1 = 0;
};
Box opaque_box(const Image& image, float alpha);

// The `box` part of `image` scaled to `width` x `height`, averaging what each new pixel covers.
Image resize(const Image& image, const Box& box, int width, int height);

// A sign's inside: everything more than `inset` pixels inside its opaque outline (alpha above 60).
std::vector<uint8_t> inside_mask(const Image& image, int inset);

} // namespace conker::menu_art
