// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "seedarc/error.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace seedarc::experimental {
// This is an experimental prototype, not a stable archive contract.
inline constexpr std::size_t archive_header_bytes = 96;
inline constexpr std::uint64_t entry_record_min_bytes = 32;
inline constexpr std::uint64_t block_record_bytes = 48;

struct ArchiveLimits {
    std::uint64_t max_index_bytes = 16ULL * 1024 * 1024;
    std::uint64_t max_entries = 100'000;
    std::uint64_t max_blocks = 1'000'000;
    std::uint64_t max_data_bytes = 8ULL * 1024 * 1024 * 1024;
    std::uint64_t max_total_output = 8ULL * 1024 * 1024 * 1024;
    std::uint32_t max_chunk_bytes = 4U * 1024 * 1024;
};

struct ArchiveHeader {
    std::uint16_t revision = 1;
    std::uint32_t flags = 0;
    std::uint32_t strategy_version = 0;
    std::uint16_t seed_length = 0;
    std::uint16_t checksum_id = 1;
    std::uint64_t index_bytes = 0;
    std::uint64_t entry_count = 0;
    std::uint64_t block_count = 0;
    std::uint64_t data_bytes = 0;
    std::uint64_t total_output = 0;
    std::array<std::byte, 16> seed{};
    std::uint32_t chunk_size_bytes = 1024U * 1024;

    bool operator==(const ArchiveHeader &) const = default;
};

struct BlockDescriptor {
    std::uint64_t data_offset = 0;
    std::uint64_t stored_size = 0;
    std::uint64_t raw_size = 0;
    std::uint32_t codec_id = 0;
    std::uint32_t codec_config_id = 0;
    std::uint32_t raw_checksum = 0;
};

[[nodiscard]] std::uint32_t crc32c(std::span<const std::byte> bytes) noexcept;

[[nodiscard]] std::expected<std::array<std::byte, archive_header_bytes>, Error>
encode_header(const ArchiveHeader &header, const ArchiveLimits &limits = {});

[[nodiscard]] std::expected<ArchiveHeader, Error> decode_header(std::span<const std::byte> bytes,
                                                                const ArchiveLimits &limits = {});

// Descriptors are supplied by the caller; this does not parse an index or decode blocks.
[[nodiscard]] std::expected<void, Error>
validate_block_layout(const ArchiveHeader &header, std::span<const BlockDescriptor> blocks,
                      const ArchiveLimits &limits = {});
} // namespace seedarc::experimental
