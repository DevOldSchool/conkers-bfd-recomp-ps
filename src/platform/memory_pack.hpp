// An RT64 texture pack held in memory: the built-in packs the game makes from the player's ROM
// (src/enhancements/sharp_font, src/enhancements/sharp_menus). Loaded through
// tools/patches/rt64-memory-packs.patch (ReplacementDirectory::fileSystem).
#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "common/rt64_filesystem.h"
#include "stb/stb_image_write.h"

namespace conker::memory_pack {

using Files = std::map<std::string, std::vector<uint8_t>>;

struct FileIterator : RT64::FileSystemIteratorImplementation {
    Files::const_iterator it;

    explicit FileIterator(Files::const_iterator it) : it(it) {}
    const std::string& value() override { return it->first; }
    void increment() override { ++it; }
    bool compare(const FileSystemIteratorImplementation* other) const override {
        return it == static_cast<const FileIterator*>(other)->it;
    }
};

struct Pack : RT64::FileSystem {
    Files files;
    std::string textures;  // rt64.json's texture entries so far

    Iterator begin() const override { return {std::make_shared<FileIterator>(files.begin())}; }
    Iterator end() const override { return {std::make_shared<FileIterator>(files.end())}; }
    bool load(const std::string& path, uint8_t* data, size_t max_bytes) const override {
        auto it = files.find(path);
        if (it == files.end() || it->second.size() > max_bytes) return false;
        std::copy(it->second.begin(), it->second.end(), data);
        return true;
    }
    size_t getSize(const std::string& path) const override {
        auto it = files.find(path);
        return it == files.end() ? 0 : it->second.size();
    }
    bool exists(const std::string& path) const override { return files.count(path) != 0; }
    std::string makeCanonical(const std::string& path) const override { return exists(path) ? path : std::string(); }

    // `rgba` (straight alpha) as PNG bytes.
    static std::vector<uint8_t> encode_png(int width, int height, const uint8_t* rgba) {
        std::vector<uint8_t> png;
        stbi_write_png_to_func(append_bytes, &png, width, height, 4, rgba, width * 4);
        return png;
    }

    // Adds `png` as the replacement for the texture with RT64 hash `hash`. Drawn without RT64's
    // half-texel shift: Super-xBR keeps each original pixel at the top left of its block, where
    // the N64 samples it.
    void add(const std::string& path, uint64_t hash, std::vector<uint8_t> png) {
        files[path] = std::move(png);
        char hash_text[17];
        std::snprintf(hash_text, sizeof(hash_text), "%016llx", static_cast<unsigned long long>(hash));
        textures += std::string(textures.empty() ? "" : ",") + "{\"path\":\"" + path + "\",\"hashes\":{\"rt64\":\"" +
                    hash_text + "\",\"rice\":\"\"},\"operation\":\"preload\",\"shift\":\"none\"}";
    }

    void add(const std::string& path, uint64_t hash, int width, int height, const uint8_t* rgba) {
        add(path, hash, encode_png(width, height, rgba));
    }

    // Writes rt64.json once every texture is added.
    void finish() {
        const std::string database =
            "{\"configuration\":{\"configurationVersion\":3,\"autoPath\":\"rt64\",\"defaultOperation\":\"stream\","
            "\"defaultShift\":\"half\",\"hashVersion\":5},\"textures\":[" + textures +
            "],\"operationFilters\":[],\"shiftFilters\":[],\"extraFiles\":[]}";
        files["rt64.json"].assign(database.begin(), database.end());
    }

    static void append_bytes(void* context, void* data, int size) {
        auto* out = static_cast<std::vector<uint8_t>*>(context);
        out->insert(out->end(), static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
    }
};

// Writes the pack's files under `dir`, to edit by hand and load from conker-recomp-data/texture_packs.
inline void write_files(const RT64::FileSystem& pack, const std::filesystem::path& dir) {
    std::vector<uint8_t> bytes;
    for (auto it = pack.begin(); it != pack.end(); ++it) {
        const std::string& name = *it;
        const std::filesystem::path path = dir / name;
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        bytes.resize(pack.getSize(name));
        if (pack.load(name, bytes.data(), bytes.size())) {
            std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        }
    }
}

} // namespace conker::memory_pack
