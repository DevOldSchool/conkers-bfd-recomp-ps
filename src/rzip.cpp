#include "rzip.hpp"

#include <stdexcept>
#include <string>

#include "miniz.h"

namespace conker::rzip {

namespace {

uint32_t read_be32(std::span<const uint8_t> buf, size_t offset) {
    if (offset + 4 > buf.size()) {
        throw std::runtime_error("RZIP read past end of buffer");
    }
    return (uint32_t(buf[offset]) << 24) | (uint32_t(buf[offset + 1]) << 16) |
           (uint32_t(buf[offset + 2]) << 8) | uint32_t(buf[offset + 3]);
}

} // namespace

std::vector<uint8_t> inflate_chunk(std::span<const uint8_t> blob) {
    if (blob.size() < 5) {
        throw std::runtime_error("RZIP chunk is shorter than its header and deflate stream");
    }
    const uint32_t declared_size = read_be32(blob, 0);

    std::vector<uint8_t> out(declared_size);
    tinfl_decompressor inflator;
    tinfl_init(&inflator);
    size_t in_bytes = blob.size() - 4;
    size_t out_bytes = out.size();
    // Raw deflate (no zlib header) into one buffer that holds the whole output, so tinfl does not
    // need its 32 KiB circular dictionary buffer.
    const tinfl_status status = tinfl_decompress(&inflator, blob.data() + 4, &in_bytes, out.data(), out.data(), &out_bytes,
                                                 TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    if (status != TINFL_STATUS_DONE) {
        throw std::runtime_error("RZIP deflate stream is truncated or corrupt (status " + std::to_string(int(status)) + ")");
    }
    if (out_bytes != declared_size) {
        throw std::runtime_error("RZIP length mismatch: header declares " + std::to_string(declared_size) +
                                 ", decoded " + std::to_string(out_bytes));
    }
    return out;
}

GameArchive parse_game_archive(std::span<const uint8_t> payload) {
    const uint32_t data_start = read_be32(payload, 0);

    // XOR-encoded code chunk offsets, terminated by a zero word.
    std::vector<uint32_t> offsets;
    for (size_t index = 1; index * 4 + 4 <= payload.size(); index++) {
        const uint32_t encoded = read_be32(payload, index * 4);
        if (encoded == 0) {
            break;
        }
        offsets.push_back(encoded ^ kOffsetXorKey);
    }
    if (offsets.size() < 2) {
        throw std::runtime_error("game RZIP code table has fewer than two offsets");
    }
    for (size_t i = 1; i < offsets.size(); i++) {
        if (offsets[i] < offsets[i - 1]) {
            throw std::runtime_error("game RZIP code offsets are not monotonic");
        }
    }
    const uint32_t code_end = offsets.back();
    if (!(code_end <= data_start && data_start < payload.size())) {
        throw std::runtime_error("game RZIP data offset is outside the archive or before code end");
    }

    GameArchive archive;
    for (size_t i = 0; i + 1 < offsets.size(); i++) {
        std::vector<uint8_t> chunk = inflate_chunk(payload.subspan(offsets[i], offsets[i + 1] - offsets[i]));
        archive.code.insert(archive.code.end(), chunk.begin(), chunk.end());
        archive.code_chunk_count++;
    }
    archive.data = inflate_chunk(payload.subspan(data_start));
    return archive;
}

} // namespace conker::rzip
