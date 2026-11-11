// SPDX-License-Identifier: GPL-3.0-or-later
// Fuzz target: experimental archive header decoding and Store block layout validation.
#include "seedarc/archive.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <vector>

using namespace seedarc::experimental;

namespace {
std::uint64_t le(std::span<const std::byte> bytes, std::size_t offset, std::size_t width) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < width && offset + i < bytes.size(); ++i) {
        value |= std::uint64_t{std::to_integer<unsigned char>(bytes[offset + i])} << (i * 8);
    }
    return value;
}

void require(bool condition) {
    if (!condition) {
        std::abort();
    }
}

// Property: any accepted header re-encodes to exactly the accepted bytes.
void check_round_trip(std::span<const std::byte> header_bytes) {
    const auto decoded = decode_header(header_bytes);
    if (!decoded) {
        return;
    }
    const auto encoded = encode_header(*decoded);
    require(encoded.has_value());
    require(std::ranges::equal(*encoded, header_bytes));
    require(decode_header(*encoded).value() == *decoded);
}

// Property: an accepted layout is contiguous from 0, within chunk limits, and sums to the header.
void check_layout(std::span<const std::byte> rest) {
    constexpr std::size_t descriptor_bytes = 36;
    constexpr std::size_t max_descriptors = 256;
    if (rest.size() < 20) {
        return;
    }
    std::vector<BlockDescriptor> blocks;
    for (std::size_t offset = 20;
         offset + descriptor_bytes <= rest.size() && blocks.size() < max_descriptors;
         offset += descriptor_bytes) {
        BlockDescriptor block;
        block.data_offset = le(rest, offset, 8);
        block.stored_size = le(rest, offset + 8, 8);
        block.raw_size = le(rest, offset + 16, 8);
        block.codec_id = static_cast<std::uint32_t>(le(rest, offset + 24, 4));
        block.codec_config_id = static_cast<std::uint32_t>(le(rest, offset + 28, 4));
        block.raw_checksum = static_cast<std::uint32_t>(le(rest, offset + 32, 4));
        blocks.push_back(block);
    }
    ArchiveHeader header;
    header.entry_count = 1;
    header.block_count = blocks.size();
    header.index_bytes = entry_record_min_bytes + block_record_bytes * blocks.size();
    header.data_bytes = le(rest, 0, 8);
    header.total_output = le(rest, 8, 8);
    header.chunk_size_bytes = static_cast<std::uint32_t>(le(rest, 16, 4));
    if (!validate_block_layout(header, blocks)) {
        return;
    }
    std::uint64_t next = 0;
    for (const auto &block : blocks) {
        require(block.data_offset == next && block.stored_size == block.raw_size);
        require(block.raw_size > 0 && block.raw_size <= header.chunk_size_bytes);
        next += block.stored_size;
    }
    require(next == header.data_bytes && next == header.total_output);
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    const auto bytes = std::as_bytes(std::span{data, size});
    (void)decode_header(bytes); // any length, must fail closed without reading out of bounds
    if (bytes.size() < archive_header_bytes) {
        return 0;
    }
    const auto head = bytes.first(archive_header_bytes);
    check_round_trip(head);
    // Repaired copy: fix the magic and CRC so mutations reach the field validation logic.
    std::array<std::byte, archive_header_bytes> repaired{};
    std::ranges::copy(head, repaired.begin());
    constexpr std::array<char, 8> magic{'S', 'A', 'R', 'C', 'E', 'X', 'P', '\0'};
    for (std::size_t i = 0; i < magic.size(); ++i) {
        repaired[i] = static_cast<std::byte>(magic[i]);
    }
    const auto crc = crc32c(std::span{repaired}.first(92));
    for (std::size_t i = 0; i < 4; ++i) {
        repaired[92 + i] = static_cast<std::byte>((crc >> (i * 8)) & 0xffU);
    }
    check_round_trip(repaired);
    check_layout(bytes.subspan(archive_header_bytes));
    return 0;
}
