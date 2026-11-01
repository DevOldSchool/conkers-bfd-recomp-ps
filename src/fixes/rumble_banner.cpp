// The title menu shows "INSERT RUMBLE PAK" for a few seconds every time it comes up, whether or
// not a pak is inserted: func_151DE8F0 draws message 0x8F while the countdown D_8008FEFC, which
// starts at 240, is above 60, and fades it out on the way down. When controller 1's pad can
// rumble (src/platform/input.hpp) the game has its rumble pak, so the countdown is cleared before
// it is read and the banner never shows. Native helper called from the [[patches.hook]] in
// config/us.toml. CONKER_RUMBLE_BANNER=1 shows it anyway.
#include <cstdint>
#include <cstdlib>

#include "recomp.h"

#include "platform/input.hpp"
#include "enhancements/status.hpp"
#include "runtime/net_play.hpp"

namespace {

inline gpr kseg0(uint32_t address) { return static_cast<gpr>(static_cast<int32_t>(address)); }

constexpr uint32_t kBannerCountdown = 0x8008FEFC;  // D_8008FEFC: s16, banner shown while > 60

bool always_show_banner() {
    static const bool show = [] {
        const char* value = std::getenv("CONKER_RUMBLE_BANNER");
        return value != nullptr && value[0] == '1';
    }();
    return show;
}

} // namespace

extern "C" void conker_rumble_banner(uint8_t* rdram, recomp_context* /*ctx*/) {
    // Online every player's port has a rumble pak (net_play.cpp), whatever this machine's pad.
    if (!always_show_banner() && (conker::net::active() || conker::input::has_rumble())) {
        MEM_H(0, kseg0(kBannerCountdown)) = 0;
    }
}

nlohmann::json conker::status::rumble_banner() {
    return enhancement("rumble_banner", "No Rumble Pak banner with a rumbling pad", !always_show_banner(),
                       "CONKER_RUMBLE_BANNER", {{"pad_can_rumble", conker::input::has_rumble()}});
}
