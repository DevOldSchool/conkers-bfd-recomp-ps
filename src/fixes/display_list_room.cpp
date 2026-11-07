// More room in each frame's display list. func_15015E80 sizes the two frame display list buffers
// (D_800BE9C8) when a scene loads, from the scene's command count (u16 +0x1C of the header D_800B0DF0
// points to; 8000 when 0) plus 400 spare, and the menus drop what they draw past the count but keep
// the spare for what follows. This port draws more than the console in some scenes (the ONLINE lobby
// page on the title, sharper text and menus, button pictures), and the title with the lobby open used
// up the spare: its commands ran past the buffer into the next heap block's header, and the allocator
// followed the damaged link and crashed (seen hosting a second session). Native helper called from the
// [[patches.hook]] snippet for func_15015E80 in config/us.toml.
#include <cstdint>

#include "recomp.h"

namespace {

constexpr uint32_t kSceneHeader = 0x800B0DF0;  // D_800B0DF0: the loaded scene's header
constexpr uint32_t kCommandCount = 0x1C;       // u16: its frame display list's command count
constexpr uint32_t kDefaultCount = 0x1F40;     // what func_15015E80 uses for 0
constexpr uint32_t kExtraCommands = 0x200;     // 4 KiB more a buffer, 8 KiB a scene

inline gpr kseg0(uint32_t address) { return static_cast<gpr>(static_cast<int32_t>(address)); }

}  // namespace

// At func_15015E80's start: the count it reads grown by kExtraCommands.
extern "C" void conker_display_list_room(uint8_t* rdram, recomp_context* /*ctx*/) {
    const uint32_t header = static_cast<uint32_t>(MEM_W(0, kseg0(kSceneHeader)));
    if (header < 0x80000000u || header >= 0x80800000u) {
        return;
    }
    const uint32_t count = MEM_HU(kCommandCount, kseg0(header));
    MEM_H(kCommandCount, kseg0(header)) = static_cast<int16_t>((count != 0 ? count : kDefaultCount) + kExtraCommands);
}
