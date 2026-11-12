// SPDX-License-Identifier: GPL-3.0-or-later
// Fuzz target: the complete in-memory archive pipeline (header, index, block decoding) and the
// index parser on its own.
#include "seedarc/archive_index.hpp"
#include "seedarc/store_archive.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <vector>

using namespace seedarc::experimental;

namespace {
void require(bool condition) {
    if (!condition) {
        std::abort();
    }
}

// Properties of any index the parser accepts.
void check_accepted(const ArchiveHeader &header, const ArchiveIndex &index,
                    std::span<const std::byte> bytes) {
    require(index.entries.size() == header.entry_count);
    require(index.blocks.size() == header.block_count);
    require(std::ranges::equal(encode_index(index), bytes)); // the encoding is canonical
    std::uint64_t next_block = 0;
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < index.entries.size(); ++i) {
        const auto &entry = index.entries[i];
        require(i == 0 || archive_path_less(index.entries[i - 1].path, entry.path));
        require(entry.first_block == next_block);
        require(!entry.directory || (entry.size == 0 && entry.block_count == 0));
        std::uint64_t size = 0;
        for (std::uint64_t j = 0; j < entry.block_count; ++j) {
            size += index.blocks[static_cast<std::size_t>(next_block + j)].raw_size;
        }
        require(size == entry.size);
        next_block += entry.block_count;
        total += entry.size;
    }
    require(next_block == header.block_count && total == header.total_output);
}

// Interprets the input as [16 parameter bytes][index records] and repairs the index checksum, so
// that mutations reach the record validation instead of stopping at the CRC.
void parse_repaired(std::span<const std::byte> bytes) {
    if (bytes.size() < 16) {
        return;
    }
    const auto field = [&bytes](std::size_t offset) {
        return std::to_integer<std::uint32_t>(bytes[offset]) |
               (std::to_integer<std::uint32_t>(bytes[offset + 1]) << 8);
    };
    std::vector<std::byte> index(bytes.begin() + 16, bytes.end());
    const auto crc = crc32c(index);
    for (std::size_t i = 0; i < index_trailer_bytes; ++i) {
        index.push_back(static_cast<std::byte>((crc >> (i * 8)) & 0xffU));
    }
    ArchiveHeader header;
    header.entry_count = field(0) % 64;
    header.block_count = field(2) % 64;
    header.total_output = field(4);
    header.data_bytes = header.total_output;
    header.chunk_size_bytes = 1 + field(6) % 4096;
    header.index_bytes = header.entry_count == 0 ? 0 : index.size();
    const std::span<const std::byte> view =
        header.entry_count == 0 ? std::span<const std::byte>{} : std::span{index};
    if (const auto parsed = parse_index(header, view)) {
        check_accepted(header, *parsed, view);
    }
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    const auto bytes = std::as_bytes(std::span{data, size});
    // Whole archive: must fail closed or succeed without reading out of bounds.
    if (const auto summary = verify_archive_bytes(bytes)) {
        require(summary->archive_bytes == bytes.size());
        const auto header = decode_header(bytes.first(archive_header_bytes));
        require(header.has_value());
        const auto index_bytes = bytes.last(static_cast<std::size_t>(header->index_bytes));
        const auto index = parse_index(*header, index_bytes);
        require(index.has_value());
        check_accepted(*header, *index, index_bytes);
    }
    parse_repaired(bytes);
    return 0;
}
