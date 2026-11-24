// Sharper menus: the menus' lettered signs and headings, enlarged from the player's ROM, as a
// built-in RT64 texture pack.
#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

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

// The user's packs' large skull fitted over the death skull, for packs without a death skull of
// their own: loaded after them. Nothing if it is not wanted or cannot be made.
std::shared_ptr<RT64::FileSystem> pack_death_skull(const std::vector<std::filesystem::path>& user_packs);

// The menu sign or picture sprite resource `resource` is a tile of, as "GAME1 2/3": its name and
// which tile of it. Empty if it is none of the ones this pack knows. For the object inspector.
std::string sprite_name(int resource);

} // namespace conker::sharp_menus
