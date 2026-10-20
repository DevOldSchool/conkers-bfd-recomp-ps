// Sharper menus: the menus' lettered signs and headings, enlarged from the player's ROM, as a
// built-in RT64 texture pack.
#pragma once

#include <memory>

namespace RT64 {
struct FileSystem;
}

namespace conker::sharp_menus {

// Whether the built-in menus pack is wanted (CONKER_SHARP_MENUS, on unless "0").
bool enabled();

// An RT64 texture pack, in memory, that replaces the menus' lettered tiles with versions four times
// larger, made from the ROM. Null if the ROM's menu art is not where it should be.
std::shared_ptr<RT64::FileSystem> make_pack();

// With CONKER_DUMP_SHARP_MENUS=<dir>, writes the pack's files to <dir> (a pack to start from).
void dump_pack(const RT64::FileSystem& pack);

} // namespace conker::sharp_menus
