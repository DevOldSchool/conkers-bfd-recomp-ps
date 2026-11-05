// Conker kept out of the widened view of the intro while he waits for his cue. Native helpers
// called from the [[patches.hook]] snippets in config/us.toml (func_1502C974, func_150311C4).
//
// Scene 0x21 is the intro after the boot logos: the N64 logo under a spotlight, Conker attacking
// it and the Rareware logo. Through its first shots, while the camera moves around the N, Conker
// (actor 0) stands at (0, 0, -491), just past the right edge of the 4:3 view, until the shot cuts
// and he is moved on. A widened view showed him standing there. In widescreen neither he nor his
// attachments (the chainsaw, a glove, drawn separately on their parent actor) are drawn while he
// is on that spot; in 4:3 nothing changes.
#include <cmath>
#include <cstdint>
#include <cstring>

#include "recomp.h"
#include "platform/display_mode.hpp"

namespace {

inline gpr kseg0(uint32_t address) { return static_cast<gpr>(static_cast<int32_t>(address)); }

constexpr uint32_t kScene = 0x800BE9F0;    // D_800BE9F0: the scene being played
constexpr uint32_t kIntroScene = 0x21;
constexpr uint32_t kConker = 0x800CC2D0;   // D_800CC2D0: the actors, 0x32C bytes each; Conker is 0
constexpr int kActorModel = 0x04;          // applied model byte; Conker's is 0
constexpr int kActorX = 0x14;              // position, floats
constexpr float kWaitX = 0.0f, kWaitY = 0.0f, kWaitZ = -491.0f;

float read_float(uint8_t* rdram, uint32_t address) {
    const uint32_t bits = MEM_W(0, kseg0(address));
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// Whether Conker is waiting off the 4:3 view in the intro and the picture is widened.
bool conker_waiting(uint8_t* rdram) {
    if (!conker::display::logic_widescreen() || MEM_W(0, kseg0(kScene)) != kIntroScene ||
        MEM_BU(0, kseg0(kConker + kActorModel)) != 0) {
        return false;
    }
    const float x = read_float(rdram, kConker + kActorX);
    const float y = read_float(rdram, kConker + kActorX + 4);
    const float z = read_float(rdram, kConker + kActorX + 8);
    return std::fabs(x - kWaitX) < 1.0f && std::fabs(y - kWaitY) < 1.0f && std::fabs(z - kWaitZ) < 1.0f;
}

}  // namespace

// At func_1502C974's entry, which draws actor $a1 (`index`) into the display list at $a0 and
// returns the display list's new end: nonzero to return $a0 without drawing it.
extern "C" int conker_widescreen_hide_actor(uint8_t* rdram, gpr index) {
    return static_cast<int32_t>(index) == 0 && conker_waiting(rdram);
}

// At func_150311C4's entry, which draws an attachment on its parent actor $a2 (`parent`, the
// actor's address) into the display list at $a0 and returns the display list's new end: nonzero
// to return $a0 without drawing it.
extern "C" int conker_widescreen_hide_attachment(uint8_t* rdram, gpr parent) {
    return static_cast<uint32_t>(parent) == kConker && conker_waiting(rdram);
}
