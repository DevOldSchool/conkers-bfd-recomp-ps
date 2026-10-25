// The PlayStation square the B pads get where their B was: N64 B is pressed by the pad's left face
// button (Square) as well as its right one (src/platform/input_sdl.cpp), and Square is the one
// the pads show. It is a square with rounded corners, measured as a circle is: `reach` is to it
// what the distance from the middle is to a circle (a texel `reach` r out lies on the square of
// half-width r), so a stroke between two reaches is as wide along each side as between two radii,
// and `facing` is the way its edge there faces, as the direction out from the middle is a circle's.
#pragma once

#include <cmath>

namespace conker::pad_icons::square_symbol {

// How square: the exponent of the superellipse |x|^p + |y|^p = r^p (2 is a circle).
constexpr float kSquareness = 8.0f;

inline float reach(float dx, float dy) {
    return std::pow(std::pow(std::abs(dx), kSquareness) + std::pow(std::abs(dy), kSquareness), 1 / kSquareness);
}

struct Facing {
    float x, y;  // a unit vector
};

// Which way the square's edge faces at (dx, dy): out from the middle, square to its sides.
inline Facing facing(float dx, float dy) {
    const float gx = std::copysign(std::pow(std::abs(dx), kSquareness - 1), dx);
    const float gy = std::copysign(std::pow(std::abs(dy), kSquareness - 1), dy);
    const float length = std::hypot(gx, gy);
    if (length == 0) return {0, 0};
    return {gx / length, gy / length};
}

// The angle `facing` points at, as std::atan2(dy, dx) is a circle's.
inline float turn(float dx, float dy) {
    const Facing f = facing(dx, dy);
    return std::atan2(f.y, f.x);
}

} // namespace conker::pad_icons::square_symbol
