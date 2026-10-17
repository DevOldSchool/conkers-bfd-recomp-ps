// Conker's RZIP containers: a four-byte big-endian length followed by a raw deflate stream.
// The game archive (ROM 0x42450 in the US release) stores the game overlay's code as 4 KiB
// chunks behind an XOR-encoded offset table, followed by one chunk holding the game data.
// Format documented by the decomp in docs/rzip-assets.md and scripts/rzip_archive.py.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace conker::rzip {

constexpr uint32_t kOffsetXorKey = 0x8039CCCA;

// Inflates one length-prefixed raw deflate chunk. Throws std::runtime_error on malformed input.
std::vector<uint8_t> inflate_chunk(std::span<const uint8_t> blob);

struct GameArchive {
    std::vector<uint8_t> code;  // decompressed game code image (loads at 0x15000000)
    std::vector<uint8_t> data;  // decompressed game data image (loads at 0x80082B20)
    size_t code_chunk_count = 0;
};

// Parses and inflates the whole game archive. `payload` starts at the archive's first word.
GameArchive parse_game_archive(std::span<const uint8_t> payload);

} // namespace conker::rzip
