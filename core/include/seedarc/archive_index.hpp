// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "seedarc/archive.hpp"
#include "seedarc/error.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace seedarc::experimental {
// Experimental index wire format (ADR-0002, revision 1 of the container; not frozen).
//
// An archive is [96-byte header][data_bytes of block data][index_bytes of index]. The index is
// empty when the archive has no entries; otherwise it is
//   entry records (in canonical order) | block records (48 bytes each) | CRC32C of all of that.
// Entry record: u32 path_bytes, u8 kind (0 file, 1 directory), 3 zero bytes, u64 size,
//   u64 first_block, u64 block_count, then path_bytes of UTF-8 path.
// Block record: u64 data_offset, u64 stored_size, u64 raw_size, u32 codec_id,
//   u32 codec_config_id, u32 raw CRC32C, 12 zero bytes.
// All integers are little-endian.
inline constexpr std::size_t entry_record_fixed_bytes = 32;
inline constexpr std::size_t index_trailer_bytes = 4;
static_assert(entry_record_fixed_bytes == entry_record_min_bytes);

struct IndexEntry {
    std::string path; // UTF-8, '/' separated, validated by the Windows NFC path profile
    bool directory = false;
    std::uint64_t size = 0;
    std::uint64_t first_block = 0;
    std::uint64_t block_count = 0;

    bool operator==(const IndexEntry &) const = default;
};

struct ArchiveIndex {
    std::vector<IndexEntry> entries;
    std::vector<BlockDescriptor> blocks;

    bool operator==(const ArchiveIndex &) const = default;
};

// Canonical entry order: paths compared component by component, each component by UTF-8 bytes
// (equivalently: byte order with '/' sorting before every other byte). This is a depth-first
// pre-order, so a directory is immediately followed by its whole subtree.
[[nodiscard]] bool archive_path_less(std::string_view left, std::string_view right) noexcept;

// Size encode_index() will produce; fails only on arithmetic overflow.
[[nodiscard]] std::expected<std::uint64_t, Error> encoded_index_bytes(const ArchiveIndex &index);

// Pure serialization: does not validate. Writers must pass the result through parse_index().
[[nodiscard]] std::vector<std::byte> encode_index(const ArchiveIndex &index);

// Complete validation of an untrusted index against its (already decoded) header:
// record bounds, CRC32C, canonical order, path safety and collisions, explicit parent
// directories, and the file-to-block mapping (every block owned by exactly one file, in order,
// contiguous, fixed chunking, sizes adding up to the header totals).
[[nodiscard]] std::expected<ArchiveIndex, Error> parse_index(const ArchiveHeader &header,
                                                             std::span<const std::byte> bytes,
                                                             const ArchiveLimits &limits = {});
} // namespace seedarc::experimental
