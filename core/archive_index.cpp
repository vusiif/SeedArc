// SPDX-License-Identifier: GPL-3.0-or-later
#include "seedarc/archive_index.hpp"

#include "seedarc/archive_path.hpp"
#include "seedarc/checked_arithmetic.hpp"

#include <algorithm>
#include <unordered_set>

namespace seedarc::experimental {
namespace {
constexpr std::size_t block_reserved_offset = 36;

std::uint64_t read_le(std::span<const std::byte> bytes, std::size_t offset,
                      std::size_t width) noexcept {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < width; ++i) {
        value |= std::uint64_t{std::to_integer<unsigned char>(bytes[offset + i])} << (i * 8);
    }
    return value;
}

void append_le(std::vector<std::byte> &bytes, std::uint64_t value, std::size_t width) {
    for (std::size_t i = 0; i < width; ++i) {
        bytes.push_back(static_cast<std::byte>((value >> (i * 8)) & 0xffU));
    }
}

std::unexpected<Error> malformed(std::string message) {
    return std::unexpected(Error{ErrorCode::malformed_archive, std::move(message)});
}

std::unexpected<Error> entry_error(std::size_t index, const char *message) {
    return malformed("index entry " + std::to_string(index) + ": " + message);
}

unsigned order_key(char value) noexcept {
    return value == '/' ? 0U : static_cast<unsigned>(static_cast<unsigned char>(value)) + 1U;
}
} // namespace

bool archive_path_less(std::string_view left, std::string_view right) noexcept {
    return std::ranges::lexicographical_compare(left, right, {}, order_key, order_key);
}

std::expected<std::uint64_t, Error> encoded_index_bytes(const ArchiveIndex &index) {
    if (index.entries.empty() && index.blocks.empty()) {
        return 0;
    }
    auto total = checked_multiply(index.blocks.size(), block_record_bytes);
    for (const auto &entry : index.entries) {
        if (!total) {
            break;
        }
        total = checked_add(*total, entry_record_fixed_bytes);
        if (total) {
            total = checked_add(*total, entry.path.size());
        }
    }
    if (total) {
        total = checked_add(*total, index_trailer_bytes);
    }
    return total;
}

std::vector<std::byte> encode_index(const ArchiveIndex &index) {
    std::vector<std::byte> bytes;
    if (index.entries.empty() && index.blocks.empty()) {
        return bytes;
    }
    if (const auto size = encoded_index_bytes(index);
        size && *size <= static_cast<std::size_t>(-1)) {
        bytes.reserve(static_cast<std::size_t>(*size));
    }
    for (const auto &entry : index.entries) {
        append_le(bytes, entry.path.size(), 4);
        append_le(bytes, entry.directory ? 1U : 0U, 1);
        append_le(bytes, 0, 3);
        append_le(bytes, entry.size, 8);
        append_le(bytes, entry.first_block, 8);
        append_le(bytes, entry.block_count, 8);
        const auto path = std::as_bytes(std::span{entry.path});
        bytes.insert(bytes.end(), path.begin(), path.end());
    }
    for (const auto &block : index.blocks) {
        append_le(bytes, block.data_offset, 8);
        append_le(bytes, block.stored_size, 8);
        append_le(bytes, block.raw_size, 8);
        append_le(bytes, block.codec_id, 4);
        append_le(bytes, block.codec_config_id, 4);
        append_le(bytes, block.raw_checksum, 4);
        append_le(bytes, 0, 8);
        append_le(bytes, 0, 4);
    }
    append_le(bytes, crc32c(bytes), index_trailer_bytes);
    return bytes;
}

std::expected<ArchiveIndex, Error> parse_index(const ArchiveHeader &header,
                                               std::span<const std::byte> bytes,
                                               const ArchiveLimits &limits) {
    // Limits first: after this, entry_count and block_count are within limits and their minimum
    // records fit in index_bytes, so every reservation below is bounded by bytes actually present.
    if (auto valid = validate_header(header, limits); !valid) {
        return std::unexpected(valid.error());
    }
    if (bytes.size() != header.index_bytes) {
        return malformed("index length does not match the archive header");
    }
    ArchiveIndex index;
    if (header.entry_count == 0) {
        return index; // header validation guarantees an empty index and no blocks or data
    }
    if (bytes.size() < index_trailer_bytes) {
        return malformed("index is too short for its checksum");
    }
    const auto body = bytes.first(bytes.size() - index_trailer_bytes);
    if (read_le(bytes, body.size(), index_trailer_bytes) != crc32c(body)) {
        return std::unexpected(Error{ErrorCode::integrity_error, "archive index CRC32C mismatch"});
    }

    const auto entry_count = static_cast<std::size_t>(header.entry_count);
    const auto block_count = static_cast<std::size_t>(header.block_count);
    index.entries.reserve(entry_count);
    std::size_t cursor = 0;
    std::uint64_t next_block = 0;
    std::uint64_t total_output = 0;
    for (std::size_t i = 0; i < entry_count; ++i) {
        if (body.size() - cursor < entry_record_fixed_bytes) {
            return entry_error(i, "record is truncated");
        }
        const auto path_bytes = read_le(body, cursor, 4);
        const auto kind = read_le(body, cursor + 4, 1);
        const auto reserved = read_le(body, cursor + 5, 3);
        IndexEntry entry;
        entry.size = read_le(body, cursor + 8, 8);
        entry.first_block = read_le(body, cursor + 16, 8);
        entry.block_count = read_le(body, cursor + 24, 8);
        cursor += entry_record_fixed_bytes;
        if (path_bytes == 0 || path_bytes > body.size() - cursor) {
            return entry_error(i, "path length is zero or exceeds the index");
        }
        if (kind > 1 || reserved != 0) {
            return entry_error(i, "unknown entry kind or nonzero reserved bytes");
        }
        entry.directory = kind == 1;
        const auto path = body.subspan(cursor, static_cast<std::size_t>(path_bytes));
        entry.path.resize(path.size());
        std::ranges::transform(path, entry.path.begin(),
                               [](std::byte value) { return std::to_integer<char>(value); });
        cursor += path.size();

        if (entry.directory && (entry.size != 0 || entry.block_count != 0)) {
            return entry_error(i, "directory declares data");
        }
        const auto expected_blocks = entry.size / header.chunk_size_bytes +
                                     (entry.size % header.chunk_size_bytes != 0 ? 1U : 0U);
        if (entry.block_count != expected_blocks) {
            return entry_error(i, "block count does not match size and chunk size");
        }
        if (entry.first_block != next_block ||
            entry.block_count > header.block_count - next_block) {
            return entry_error(i, "block reference is dangling, overlapping or out of order");
        }
        next_block += entry.block_count;
        if (entry.size > header.total_output - total_output) {
            return entry_error(i, "sizes exceed the declared total output");
        }
        total_output += entry.size;
        if (i != 0 && !archive_path_less(index.entries.back().path, entry.path)) {
            return entry_error(i, "duplicate path or entries not in canonical order");
        }
        index.entries.push_back(std::move(entry));
    }
    if (next_block != header.block_count || total_output != header.total_output) {
        return malformed("entries do not account for every block and output byte");
    }
    if (body.size() - cursor != block_count * block_record_bytes) {
        return malformed("index has undeclared bytes or a truncated block table");
    }

    index.blocks.reserve(block_count);
    for (std::size_t i = 0; i < block_count; ++i, cursor += block_record_bytes) {
        BlockDescriptor block;
        block.data_offset = read_le(body, cursor, 8);
        block.stored_size = read_le(body, cursor + 8, 8);
        block.raw_size = read_le(body, cursor + 16, 8);
        block.codec_id = static_cast<std::uint32_t>(read_le(body, cursor + 24, 4));
        block.codec_config_id = static_cast<std::uint32_t>(read_le(body, cursor + 28, 4));
        block.raw_checksum = static_cast<std::uint32_t>(read_le(body, cursor + 32, 4));
        if (read_le(body, cursor + block_reserved_offset, 8) != 0 ||
            read_le(body, cursor + block_reserved_offset + 8, 4) != 0) {
            return malformed("block " + std::to_string(i) + ": nonzero reserved bytes");
        }
        index.blocks.push_back(block);
    }
    if (auto layout = validate_block_layout(header, index.blocks, limits); !layout) {
        return std::unexpected(layout.error());
    }
    // Fixed chunking: every block of a file is a full chunk except the last one.
    for (std::size_t i = 0; i < index.entries.size(); ++i) {
        const auto &entry = index.entries[i];
        for (std::uint64_t j = 0; j < entry.block_count; ++j) {
            const auto expected =
                j + 1 < entry.block_count
                    ? std::uint64_t{header.chunk_size_bytes}
                    : entry.size - (entry.block_count - 1) * header.chunk_size_bytes;
            if (index.blocks[static_cast<std::size_t>(entry.first_block + j)].raw_size !=
                expected) {
                return entry_error(i, "block sizes do not follow the fixed chunking");
            }
        }
    }

    // Path safety, case-insensitive collisions and file-used-as-directory conflicts.
    std::vector<PathEntry> manifest;
    manifest.reserve(index.entries.size());
    for (const auto &entry : index.entries) {
        manifest.push_back({entry.path, entry.directory});
    }
    if (auto paths = validate_manifest_paths(manifest); !paths) {
        return std::unexpected(paths.error());
    }
    // Every parent directory is an explicit earlier entry, so extraction never has to invent
    // directories and the creation list is exactly the entry list.
    std::unordered_set<std::string_view> directories;
    for (std::size_t i = 0; i < index.entries.size(); ++i) {
        const std::string_view path{index.entries[i].path};
        if (const auto slash = path.rfind('/');
            slash != std::string_view::npos && !directories.contains(path.substr(0, slash))) {
            return entry_error(i, "parent directory entry is missing");
        }
        if (index.entries[i].directory) {
            directories.insert(path);
        }
    }
    return index;
}
} // namespace seedarc::experimental
