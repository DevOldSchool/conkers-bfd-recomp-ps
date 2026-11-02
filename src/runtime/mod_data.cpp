// A little data of a mod's own kept between runs (best times, say): conker_mod_data_load and
// conker_mod_data_save, imported by mods from "*" (docs/modding.md). librecomp lets a mod change the
// game's save file but keep nothing else. Each mod gets one file, saves/mods/<mod id>.bin in the data
// folder, of up to kMaxSize bytes, named by the loader's id for the calling mod, never by the mod.
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "recomp.h"
#include "librecomp/game.hpp"
#include "librecomp/mods.hpp"
#include "librecomp/overlays.hpp"

namespace {

constexpr uint32_t kMaxSize = 16 * 1024;

// The mod's file, or none for an id that is not a plain file name (ids come from manifests).
std::filesystem::path data_path(size_t mod_index) {
    const std::string id = recomp::mods::get_mod_id(mod_index);
    if (id.empty() || id.size() > 64) {
        return {};
    }
    for (const char c : id) {
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                           c == '-' || c == '.';
        if (!plain || id == "." || id == "..") {
            return {};
        }
    }
    return recomp::get_config_path() / "saves" / "mods" / (id + ".bin");
}

// u32 conker_mod_data_load(void* data, u32 size): fills data with up to size bytes of the mod's
// file and returns how many there were (0 with none yet).
void conker_mod_data_load(uint8_t* rdram, recomp_context* ctx, size_t mod_index) {
    const gpr data = ctx->r4;
    const uint32_t size = std::min(static_cast<uint32_t>(ctx->r5), kMaxSize);
    ctx->r2 = 0;
    const std::filesystem::path path = data_path(mod_index);
    if (path.empty()) {
        return;
    }
    std::ifstream file(path, std::ios::binary);
    std::vector<char> bytes(size);
    file.read(bytes.data(), size);
    const uint32_t read = static_cast<uint32_t>(file.gcount());
    for (uint32_t i = 0; i < read; i++) {
        MEM_B(static_cast<int32_t>(i), data) = bytes[i];
    }
    ctx->r2 = read;
}

// u32 conker_mod_data_save(const void* data, u32 size): replaces the mod's file with size bytes
// (at most 16 KiB) and returns 1, or 0 when it could not.
void conker_mod_data_save(uint8_t* rdram, recomp_context* ctx, size_t mod_index) {
    const gpr data = ctx->r4;
    const uint32_t size = static_cast<uint32_t>(ctx->r5);
    ctx->r2 = 0;
    const std::filesystem::path path = data_path(mod_index);
    if (path.empty() || size > kMaxSize) {
        std::printf("[conker] mod %s: data not saved\n", recomp::mods::get_mod_id(mod_index).c_str());
        return;
    }
    std::vector<char> bytes(size);
    for (uint32_t i = 0; i < size; i++) {
        bytes[i] = static_cast<char>(MEM_B(static_cast<int32_t>(i), data));
    }
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    // Written beside it and then put in its place, so a crash midway keeps the old file.
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file.write(bytes.data(), size);
        if (!file) {
            return;
        }
    }
    std::filesystem::rename(temporary, path, error);
    ctx->r2 = error ? 0 : 1;
}

}  // namespace

namespace conker {

void register_mod_data_exports() {
    recomp::overlays::register_ext_base_export("conker_mod_data_load", conker_mod_data_load);
    recomp::overlays::register_ext_base_export("conker_mod_data_save", conker_mod_data_save);
}

}  // namespace conker
