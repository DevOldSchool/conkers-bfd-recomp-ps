// Surround as the default sound option. Native helper called from the [[patches.hook]] snippet in
// config/us.toml (func_151DD9E4). CONKER_SURROUND_DEFAULT=0 keeps the game's stereo default.
#include <cstdint>
#include <cstdlib>

#include "recomp.h"

namespace {

inline gpr kseg0(uint32_t address) { return static_cast<gpr>(static_cast<int32_t>(address)); }

// D_800E0BE0[9]: the OPTIONS menu's sound setting. func_151E7E9C passes it to
// alSurround_OutputType: 0 stereo, 1 surround, 2 mono.
constexpr uint32_t kSoundOption = 0x800E0BE9;
constexpr int8_t kSurround = 1;
constexpr int8_t kSoundOptionMax = 2;  // D_8008FE84[9]; the minimum, D_8008FE6C[9], is 0

bool surround_default_enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("CONKER_SURROUND_DEFAULT");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}

}  // namespace

// The options are kept in EEPROM block 0 behind a checksum. func_15007440 loads them at boot and
// fills them with -1 when the checksum fails (a new save, or none), then func_151DD9E4 replaces each
// field outside its range with that field's default from D_8008FE54, which for sound is stereo.
// Called at the start of func_151DD9E4: a sound field it would reset becomes surround instead, so a
// sound option the player saved is kept.
extern "C" void conker_surround_default(uint8_t* rdram, recomp_context* /*ctx*/) {
    if (!surround_default_enabled()) {
        return;
    }
    const int8_t sound = MEM_B(0, kseg0(kSoundOption));
    if (sound < 0 || sound > kSoundOptionMax) {
        MEM_B(0, kseg0(kSoundOption)) = kSurround;
    }
}
