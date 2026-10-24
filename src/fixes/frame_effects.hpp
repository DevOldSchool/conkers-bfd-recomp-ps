// Conker's colour-framebuffer effects, made by RT64 instead of the CPU (src/fixes/frame_effects.cpp).
#pragma once

#include <cstdint>
#include <vector>

namespace conker::frame_effects {

enum class Type { Mosaic = 0, Lens = 1, Grey = 2 };

// In the frame's pixels.
struct Mosaic {
    uint16_t left, top, right, bottom;  // inclusive
    uint32_t step;                      // 16.16
};

struct Lens {
    float center_x, center_y;  // where it is drawn
    float source_x, source_y;  // where it looks
    float radius, strength;
};

struct Effect {
    Type type = Type::Grey;
    Mosaic mosaic{};
    Lens lens{};
};

// RT64 makes the effects: the game's CPU passes are skipped.
void use_gpu(bool on);

// For the graphics task with display list `data_ptr`: the effects the game will run on its frame at
// its swap, in the order it runs them, into `effects`, and the RDRAM address of that frame's colour
// image (0 if none).
uint32_t frame_effects(const uint8_t* rdram, uint32_t data_ptr, std::vector<Effect>* effects);

}  // namespace conker::frame_effects
