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

// Candidate defaults (ADR-0001). They are chosen to be simultaneously reachable: the minimum
// record budget of max_entries entries plus max_blocks blocks fits in max_index_bytes
// (100000 * 32 + 262144 * 48 = 15782912 <= 16 MiB). See limits_are_coherent().
struct ArchiveLimits {
    std::uint64_t max_index_bytes = 16ULL * 1024 * 1024;
    std::uint64_t max_entries = 100'000;
    std::uint64_t max_blocks = 262'144;
    std::uint64_t max_data_bytes = 8ULL * 1024 * 1024 * 1024;
    std::uint64_t max_total_output = 8ULL * 1024 * 1024 * 1024;
    std::uint32_t max_chunk_bytes = 4U * 1024 * 1024;
};

// True when every individual limit can be reached at the same time as the others with respect
// to the index record budget, i.e. no limit is silently dead. Overflow-safe. Limits that fail
// this are still enforced field by field (tests use deliberately unbounded limits).
[[nodiscard]] constexpr bool limits_are_coherent(const ArchiveLimits &limits) noexcept {
    constexpr auto max = static_cast<std::uint64_t>(-1);
    if (limits.max_entries > max / entry_record_min_bytes ||
        limits.max_blocks > max / block_record_bytes) {
        return false;
    }
    const auto entries = limits.max_entries * entry_record_min_bytes;
    const auto blocks = limits.max_blocks * block_record_bytes;
    return entries <= max - blocks && entries + blocks <= limits.max_index_bytes &&
           limits.max_chunk_bytes != 0;
}
static_assert(limits_are_coherent(ArchiveLimits{}), "default archive limits must be coherent");

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

    bool operator==(const BlockDescriptor &) const = default;
};

// Incremental CRC32C (ADR-0001 parameters) for data that is produced or read in pieces.
class Crc32c {
  public:
    void update(std::span<const std::byte> bytes) noexcept;
    [[nodiscard]] std::uint32_t value() const noexcept { return ~state_; }

  private:
    std::uint32_t state_ = 0xffffffffU;
};

[[nodiscard]] std::uint32_t crc32c(std::span<const std::byte> bytes) noexcept;

// Field and limit checks shared by encode_header / decode_header and by index parsing.
[[nodiscard]] std::expected<void, Error> validate_header(const ArchiveHeader &header,
                                                         const ArchiveLimits &limits = {});

[[nodiscard]] std::expected<std::array<std::byte, archive_header_bytes>, Error>
encode_header(const ArchiveHeader &header, const ArchiveLimits &limits = {});

[[nodiscard]] std::expected<ArchiveHeader, Error> decode_header(std::span<const std::byte> bytes,
                                                                const ArchiveLimits &limits = {});

// Descriptors are supplied by the caller; this does not parse an index or decode blocks.
[[nodiscard]] std::expected<void, Error>
validate_block_layout(const ArchiveHeader &header, std::span<const BlockDescriptor> blocks,
                      const ArchiveLimits &limits = {});
} // namespace seedarc::experimental
