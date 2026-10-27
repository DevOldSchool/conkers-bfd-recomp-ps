// The countdown timer hung at the right side of the widened picture. Native helper called from the
// [[patches.hook]] snippet in config/us.toml (func_150938BC).
//
// A countdown (the timer over the little girl's bomb in scene 35, and the others the game shows
// with timer_display_set_enabled, func_15093818) is a 3D model, model 0xBA, drawn on the HUD by
// func_150938BC: hung from a chain off the top of the picture, with another chain running off its
// right side. The model is placed at (120, 129, -303) under the HUD's projection, D_800D23D0. RT64
// keeps such a model where it is in the 4:3 middle of a widened picture, so the timer hung inside
// the 4:3 picture's right side with its chain ending short of the screen's edge. Widened, it is
// moved right by as much as the right side moved, so it keeps its distance from that side and its
// chain runs off it again; in 4:3 nothing changes.
#include <cstdint>
#include <cstring>

#include "recomp.h"
#include "platform/display_mode.hpp"

namespace {

inline gpr kseg0(uint32_t address) { return static_cast<gpr>(static_cast<int32_t>(address)); }

constexpr uint32_t kProjection = 0x800D23D0;  // D_800D23D0: the HUD's projection, an N64 Mtx
constexpr float kTimerY = 129.0f, kTimerZ = -303.0f;
constexpr float kPictureWidth = 292.0f;
constexpr float kPictureHeight = 216.0f;
constexpr float kPictureCentre = kPictureWidth / 2.0f;
constexpr float kUndrawnColumns = 2.0f;  // the frame buffer's columns left out at each side

// Row `row`, column `column` of the N64 fixed-point matrix at `address`: the whole parts are
// 16 halfwords and the fractions 16 halfwords after them.
float matrix_element(uint8_t* rdram, uint32_t address, int row, int column) {
    const int index = row * 4 + column;
    const int32_t whole = MEM_H(index * 2, kseg0(address));
    const uint32_t fraction = static_cast<uint16_t>(MEM_HU(0x20 + index * 2, kseg0(address)));
    return static_cast<float>(static_cast<int32_t>(static_cast<uint32_t>(whole) << 16 | fraction)) / 65536.0f;
}

}  // namespace

// Before func_150938BC places the timer with func_150A7D00(matrix, x, y, z): `x` is $a1, a float's
// bits. Returns the bits of the x to place it at.
extern "C" uint32_t conker_widescreen_timer_x(uint8_t* rdram, uint32_t x_bits) {
    const float widening = conker::display::rendered_aspect() / (kPictureWidth / kPictureHeight);
    if (!conker::display::rendered_widescreen() || widening <= 1.001f) {
        return x_bits;
    }
    float x;
    std::memcpy(&x, &x_bits, sizeof(x));
    // N64 matrices take row vectors: clip x = x m00 + y m10 + z m20 + m30, clip w alike from
    // column 3. The 4:3 picture's sides are clip x / clip w = -1 and 1.
    const float m00 = matrix_element(rdram, kProjection, 0, 0);
    if (m00 == 0.0f) {
        return x_bits;
    }
    const float w = x * matrix_element(rdram, kProjection, 0, 3) + kTimerY * matrix_element(rdram, kProjection, 1, 3) +
                    kTimerZ * matrix_element(rdram, kProjection, 2, 3) + matrix_element(rdram, kProjection, 3, 3);
    // The visible right side moves out by this much, in the projection's units.
    const float shift = (kPictureCentre - kUndrawnColumns) * (widening - 1.0f) / kPictureCentre;
    x += shift * w / m00;
    uint32_t bits;
    std::memcpy(&bits, &x, sizeof(bits));
    return bits;
}
