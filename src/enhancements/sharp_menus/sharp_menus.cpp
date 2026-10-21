// The menus' signs and headings are 32x32 RGBA32 tiles (src/enhancements/sharp_menus/menu_art.cpp),
// drawn unfiltered, so at high resolutions they show as blocks. This enlarges each lettered sign
// and heading, the joystick, the speakers and the pause menu's digits four times with Super-xBR,
// keeping Rare's pixels and soft edges, and hands the tiles to RT64 as a texture pack in memory,
// keyed by the hash RT64 computes for each tile as the game loads it (TMEMHasher). Tiles drawn
// side by side are enlarged together, so no seams show between them. Other pictures (faces, the
// small statistics icons) are left as they are: they lose their detail when enlarged.
//
// The pause menu's SAVE and LOAD signs (src/enhancements/sharp_menus/pause_signs.cpp) are made at
// four times the size with the others, and keyed by the hash of the game-size tiles the pause
// menu draws.
//
// RT64 loads PNG replacements without mipmaps, so four times suits the menus' sizes at common
// resolutions better than more. CONKER_DUMP_SHARP_MENUS=<dir> writes the pack's files to <dir>;
// packs in conker-recomp-data/texture_packs load after this one and win where they replace the
// same tiles. CONKER_SHARP_MENUS=0 leaves the menus as they are.
#include "enhancements/sharp_menus/sharp_menus.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "hle/rt64_application.h"
#include "common/rt64_load_types.h"
#include "common/rt64_tmem_hasher.h"
#include "enhancements/sharp_menus/menu_art.hpp"
#include "enhancements/sharp_menus/pause_signs.hpp"
#include "platform/memory_pack.hpp"

namespace conker::sharp_menus {

namespace {

constexpr int kScale = 4;
constexpr int kTile = 32;

// First resource and tile count of each lettered sign and heading, tiles left to right. The
// sprites the menus draw through the selector table D_800859E0, then the headings drawn on their
// own (both listed in the decomp's docs/evidence/assets/interface/us_hud_menu_assets.md).
struct Title {
    int first;
    int tiles;
    const char* name;
};

constexpr Title kTitles[] = {
    // Selector table
    {2196, 1, "P1"}, {2197, 1, "P2"}, {2198, 1, "P3"}, {2199, 1, "P4"}, {2166, 1, "AI"},
    {2003, 2, "CONT... unlit"}, {2005, 2, "CONT... lit"}, {2044, 2, "QUIT unlit"}, {2046, 2, "QUIT lit"},
    {1997, 2, "CHEATS"}, {1988, 1, "OFF"}, {1989, 1, "ON"},
    {2039, 2, "PLAY unlit"}, {2041, 2, "PLAY lit"}, {2007, 2, "ERASE unlit"}, {2009, 2, "ERASE lit"},
    {2066, 2, "SET-UP unlit"}, {2068, 2, "SET-UP lit"}, {2033, 1, "N unlit"}, {2034, 1, "N lit"},
    {2095, 1, "Y unlit"}, {2096, 1, "Y lit"}, {2062, 2, "RESTART unlit"}, {2064, 2, "RESTART lit"},
    {2048, 2, "RACE A unlit"}, {2050, 2, "RACE A lit"}, {2052, 2, "RACE B unlit"}, {2054, 2, "RACE B lit"},
    {2076, 2, "TEMPLE unlit"}, {2078, 2, "TEMPLE lit"}, {2085, 2, "THE VAULT unlit"}, {2087, 2, "THE VAULT lit"},
    {1984, 2, "TOTAL WAR unlit"}, {1986, 2, "TOTAL WAR lit"}, {1999, 2, "COLORS unlit"}, {2001, 2, "COLORS lit"},
    {1990, 2, "BUNKER unlit"}, {1992, 2, "BUNKER lit"}, {2030, 3, "NEW GAME"},
    {2186, 1, "LAPS"}, {2185, 1, "KILLS"}, {2149, 1, "SCORE"},
    {2108, 3, "HUNGOVER"}, {2126, 3, "WINDY"}, {2102, 3, "BATS TOWER"}, {2099, 3, "BARN BOYS"},
    {2117, 3, "SLOPRANO"}, {2123, 3, "UGA BUGA"}, {2120, 3, "SPOOKY"}, {2111, 3, "IT'S WAR"},
    {2105, 3, "HEIST (chapter)"}, {2070, 2, "STATS"},
    {2147, 2, "REDS"}, {2129, 2, "BLUES"}, {2136, 3, "GREENS"}, {2159, 3, "YELLOWS"},
    {2150, 3, "SQUIRRELS"}, {2153, 2, "TEDIZ"}, {2162, 2, "YOU"}, {2155, 2, "UGAS"},
    {2144, 3, "RAPTORS"}, {2133, 3, "FRENCHYS"}, {2157, 2, "WIN"}, {2139, 2, "LOSE"}, {2131, 2, "DRAW"},
    // Headings
    {1977, 3, "BEACH"}, {1994, 3, "CHAPTERS"}, {2011, 3, "GAME1"}, {2014, 3, "GAME2"}, {2017, 3, "GAME3"},
    {2035, 3, "OPTIONS"}, {2056, 3, "RACE"}, {2059, 3, "RAPTOR"}, {2082, 3, "WAR"}, {2097, 2, "BACK"},
    {2180, 3, "HEIST"}, {2188, 3, "MULTI"}, {2193, 3, "PAUSED"}, {2212, 3, "TANK"},
    // Pictures that enlarge well: the menus' joystick (its six animation frames) and the OPTIONS
    // menu's speakers
    {2023, 1, "joystick 1"}, {2024, 1, "joystick 2"}, {2025, 1, "joystick 3"},
    {2026, 1, "joystick 4"}, {2027, 1, "joystick 5"}, {2028, 1, "joystick 6"},
    {2072, 2, "stereo"}, {2074, 2, "surround"}, {2029, 1, "mono"},
    // The coloured digits and dollar sign of the pause menu's money and lives (the digit table
    // D_80090074, drawn one tile per digit by func_151EADFC; the dollar sign by func_151EB06C)
    {2226, 1, "digit 0"}, {2192, 1, "digit 1"}, {2219, 1, "digit 2"}, {2216, 1, "digit 3"},
    {2178, 1, "digit 4"}, {2177, 1, "digit 5"}, {2210, 1, "digit 6"}, {2209, 1, "digit 7"},
    {2176, 1, "digit 8"}, {2191, 1, "digit 9"}, {2038, 1, "dollar"},
};

// The small P1 to P4 badges (as on the multiplayer STATS screen) are the four 16x16 corners of one
// tile, drawn one corner at a time. At 16 pixels Super-xBR has too little to go on, so each corner
// is the same badge's 32x32 version (resources 2196 to 2199) enlarged and fitted over the small
// one's outline.
constexpr int kSmallBadges = 2115;
constexpr int kLargeBadges = 2196;

// RT64's hash for a tile loaded as the game loads them: LoadBlock to TMEM 0, which puts each
// texel's red and green in the lower half and its blue and alpha in the upper half.
uint64_t hash(const menu_art::StoredTile& tile) {
    std::vector<uint8_t> tmem(4096, 0);
    for (int i = 0; i < kTile * kTile; i++) {
        tmem[i * 2] = tile[i * 4], tmem[i * 2 + 1] = tile[i * 4 + 1];
        tmem[2048 + i * 2] = tile[i * 4 + 2], tmem[2048 + i * 2 + 1] = tile[i * 4 + 3];
    }
    RT64::LoadTile load{};
    load.fmt = 0;  // G_IM_FMT_RGBA
    load.siz = 3;  // G_IM_SIZ_32b
    load.line = 8;
    return RT64::TMEMHasher::hash(tmem.data(), load, kTile, kTile, 0, RT64::TMEMHasher::CurrentHashVersion);
}

// The small badges' tile, four times larger.
std::optional<menu_art::Image> small_badges() {
    const std::optional<menu_art::Image> small = menu_art::load_row(kSmallBadges, 1);
    if (!small) return std::nullopt;
    menu_art::Image out(kTile * kScale, kTile * kScale);
    constexpr int kCorner = kTile / 2;
    for (int i = 0; i < 4; i++) {
        const int cx = (i % 2) * kCorner, cy = (i / 2) * kCorner;
        const std::optional<menu_art::Image> large = menu_art::load_row(kLargeBadges + i, 1);
        if (!large) return std::nullopt;
        menu_art::Image corner(kCorner, kCorner);
        for (int y = 0; y < kCorner; y++) std::copy_n(small->at(cx, y + cy), kCorner * 4, corner.at(0, y));
        const menu_art::Box to = menu_art::opaque_box(corner, 100);
        const menu_art::Box from = menu_art::opaque_box(*large, 100);
        if (to.x1 == 0 || from.x1 == 0) return std::nullopt;
        const menu_art::Image big = menu_art::super_xbr(*large, kScale);
        const menu_art::Image fitted = menu_art::resize(
            big, {from.x0 * kScale, from.x1 * kScale, from.y0 * kScale, from.y1 * kScale},
            (to.x1 - to.x0) * kScale, (to.y1 - to.y0) * kScale);
        for (int y = 0; y < fitted.height; y++) {
            std::copy_n(fitted.at(0, y), fitted.width * 4, out.at((cx + to.x0) * kScale, (cy + to.y0) * kScale + y));
        }
    }
    return out;
}

struct Replacement {
    std::string path;
    uint64_t hash;
    std::vector<uint8_t> png;
};

// Each 32-pixel column of the enlarged `image`, as the replacement for the tile with that hash.
std::vector<Replacement> split(const std::string& name, const menu_art::Image& image, const std::vector<uint64_t>& hashes) {
    std::vector<Replacement> out;
    const std::vector<uint8_t> rgba = menu_art::to_rgba8(image);
    const int size = kTile * kScale;
    std::vector<uint8_t> tile(size_t(size) * size * 4);
    for (size_t i = 0; i < hashes.size(); i++) {
        for (int y = 0; y < size; y++) {
            const uint8_t* row = &rgba[(size_t(y) * image.width + i * size) * 4];
            std::copy(row, row + size * 4, &tile[size_t(y) * size * 4]);
        }
        out.push_back({"menus/" + name + "_" + std::to_string(i) + ".png", hashes[i],
                       memory_pack::Pack::encode_png(size, size, tile.data())});
    }
    return out;
}

std::string file_name(const char* title) {
    std::string out;
    for (const char* c = title; *c; c++) {
        if (*c >= 'A' && *c <= 'Z') out += char(*c - 'A' + 'a');
        else if ((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9')) out += *c;
        else if (!out.empty() && out.back() != '_') out += '_';
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out;
}

} // namespace

bool enabled() {
    const char* value = std::getenv("CONKER_SHARP_MENUS");
    return value == nullptr || std::string(value) != "0";
}

void dump_pack(const RT64::FileSystem& pack) {
    const char* dir = std::getenv("CONKER_DUMP_SHARP_MENUS");
    if (dir == nullptr || dir[0] == '\0') {
        return;
    }
    memory_pack::write_files(pack, dir);
    std::printf("[conker] sharp menus written to %s\n", dir);
}

std::shared_ptr<RT64::FileSystem> make_pack() {
    // The signs are independent, so they are made on every core; SAVE and LOAD alongside.
    std::future<const pause_signs::Signs*> signs = std::async(std::launch::async, pause_signs::get);
    std::vector<std::optional<std::vector<Replacement>>> made(std::size(kTitles));
    std::atomic<size_t> next{0};
    std::vector<std::thread> workers;
    for (unsigned i = 0; i < std::max(1u, std::thread::hardware_concurrency()); i++) {
        workers.emplace_back([&] {
            for (size_t t = next++; t < std::size(kTitles); t = next++) {
                const Title& title = kTitles[t];
                const std::optional<menu_art::Image> row = menu_art::load_row(title.first, title.tiles);
                if (!row) continue;
                std::vector<uint64_t> hashes;
                for (int k = 0; k < title.tiles; k++) hashes.push_back(hash(*menu_art::load_tile(title.first + k)));
                made[t] = split(file_name(title.name), menu_art::super_xbr(*row, kScale), hashes);
            }
        });
    }
    for (std::thread& worker : workers) worker.join();

    auto pack = std::make_shared<memory_pack::Pack>();
    const size_t missing = size_t(std::count(made.begin(), made.end(), std::nullopt));
    if (missing == made.size()) {
        std::fprintf(stderr, "[conker] sharp menus: no menu art in the ROM where it should be; left as it is\n");
        return nullptr;
    }
    if (missing > 0) {
        std::fprintf(stderr, "[conker] sharp menus: %zu of %zu signs missing from the ROM\n", missing, made.size());
    }
    for (auto& title : made) {
        if (!title) continue;
        for (Replacement& r : *title) pack->add(r.path, r.hash, std::move(r.png));
    }
    if (const std::optional<menu_art::Image> badges = small_badges()) {
        for (Replacement& r : split("small_badges", *badges, {hash(*menu_art::load_tile(kSmallBadges))})) {
            pack->add(r.path, r.hash, std::move(r.png));
        }
    }
    if (const pause_signs::Signs* made_signs = signs.get()) {
        const char* names[2][2] = {{"save_unlit", "save_lit"}, {"load_unlit", "load_lit"}};
        for (int sign = 0; sign < 2; sign++) {
            for (int variant = 0; variant < 2; variant++) {
                const auto& tiles = made_signs->tiles[sign][variant];
                for (Replacement& r : split(names[sign][variant], made_signs->large[sign][variant], {hash(tiles[0]), hash(tiles[1])})) {
                    pack->add(r.path, r.hash, std::move(r.png));
                }
            }
        }
    }
    pack->finish();
    return pack;
}

} // namespace conker::sharp_menus
