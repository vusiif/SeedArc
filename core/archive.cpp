// SPDX-License-Identifier: GPL-3.0-or-later
#include "seedarc/archive.hpp"
#include "seedarc/checked_arithmetic.hpp"

#include <algorithm>
#include <string>

namespace seedarc::experimental {
namespace {
constexpr std::array magic{std::byte{'S'}, std::byte{'A'}, std::byte{'R'}, std::byte{'C'},
                           std::byte{'E'}, std::byte{'X'}, std::byte{'P'}, std::byte{0}};

template <typename T> T read_le(std::span<const std::byte> bytes, std::size_t offset) noexcept {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        value |= std::uint64_t{std::to_integer<unsigned char>(bytes[offset + i])} << (i * 8);
    }
    return static_cast<T>(value);
}

template <typename T>
void write_le(std::span<std::byte> bytes, std::size_t offset, T value) noexcept {
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        bytes[offset + i] = static_cast<std::byte>((std::uint64_t{value} >> (i * 8)) & 0xffU);
    }
}

std::expected<void, Error> validate_header(const ArchiveHeader &header,
                                           const ArchiveLimits &limits) {
    if (header.revision != 1 || header.flags != 0 || header.strategy_version != 0 ||
        header.checksum_id != 1) {
        return std::unexpected(Error{ErrorCode::unsupported_feature,
                                     "unsupported experimental archive header feature"});
    }
    if (header.seed_length != 0 && header.seed_length != 16) {
        return std::unexpected(Error{ErrorCode::malformed_archive, "invalid seed length"});
    }
    if (header.seed_length == 16) {
        return std::unexpected(
            Error{ErrorCode::unsupported_feature, "seed strategies are not implemented"});
    }
    if (std::ranges::any_of(header.seed, [](std::byte value) { return value != std::byte{0}; })) {
        return std::unexpected(
            Error{ErrorCode::malformed_archive, "nonzero bytes in unused seed field"});
    }
    if (header.chunk_size_bytes == 0) {
        return std::unexpected(Error{ErrorCode::malformed_archive, "zero archive chunk size"});
    }
    if (header.index_bytes > limits.max_index_bytes || header.entry_count > limits.max_entries ||
        header.block_count > limits.max_blocks || header.data_bytes > limits.max_data_bytes ||
        header.total_output > limits.max_total_output ||
        header.chunk_size_bytes > limits.max_chunk_bytes) {
        return std::unexpected(
            Error{ErrorCode::resource_limit, "archive header exceeds resource limits"});
    }

    const auto entries_size = checked_multiply(header.entry_count, entry_record_min_bytes);
    const auto blocks_size = checked_multiply(header.block_count, block_record_bytes);
    if (!entries_size || !blocks_size) {
        return std::unexpected(
            Error{ErrorCode::arithmetic_overflow, "archive index size multiplication overflow"});
    }
    const auto minimum_index_size = checked_add(*entries_size, *blocks_size);
    if (!minimum_index_size) {
        return std::unexpected(
            Error{ErrorCode::arithmetic_overflow, "archive index size addition overflow"});
    }
    if (*minimum_index_size > header.index_bytes) {
        return std::unexpected(
            Error{ErrorCode::malformed_archive, "archive index is shorter than declared records"});
    }
    const auto prefix_size = checked_add(archive_header_bytes, header.index_bytes);
    if (!prefix_size || !checked_add(*prefix_size, header.data_bytes)) {
        return std::unexpected(
            Error{ErrorCode::arithmetic_overflow, "archive total size overflow"});
    }
    if (header.entry_count == 0 && (header.index_bytes != 0 || header.block_count != 0 ||
                                    header.data_bytes != 0 || header.total_output != 0)) {
        return std::unexpected(
            Error{ErrorCode::malformed_archive, "empty archive contains unowned records or data"});
    }
    if (header.block_count == 0 && (header.data_bytes != 0 || header.total_output != 0)) {
        return std::unexpected(
            Error{ErrorCode::malformed_archive, "archive declares data without blocks"});
    }
    return {};
}
} // namespace

std::uint32_t crc32c(std::span<const std::byte> bytes) noexcept {
    std::uint32_t crc = 0xffffffffU;
    for (const auto value : bytes) {
        crc ^= std::to_integer<std::uint32_t>(value);
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1U) != 0 ? 0x82f63b78U : 0U);
        }
    }
    return ~crc;
}

std::expected<std::array<std::byte, archive_header_bytes>, Error>
encode_header(const ArchiveHeader &header, const ArchiveLimits &limits) {
    if (auto validation = validate_header(header, limits); !validation) {
        return std::unexpected(validation.error());
    }
    std::array<std::byte, archive_header_bytes> bytes{};
    std::ranges::copy(magic, bytes.begin());
    write_le(bytes, 8, header.revision);
    write_le(bytes, 10, static_cast<std::uint16_t>(archive_header_bytes));
    write_le(bytes, 12, header.flags);
    write_le(bytes, 16, header.strategy_version);
    write_le(bytes, 20, header.seed_length);
    write_le(bytes, 22, header.checksum_id);
    write_le(bytes, 24, header.index_bytes);
    write_le(bytes, 32, header.entry_count);
    write_le(bytes, 40, header.block_count);
    write_le(bytes, 48, header.data_bytes);
    write_le(bytes, 56, header.total_output);
    std::ranges::copy(header.seed, bytes.begin() + 64);
    write_le(bytes, 80, header.chunk_size_bytes);
    write_le(bytes, 92, crc32c(std::span{bytes}.first(92)));
    return bytes;
}

std::expected<ArchiveHeader, Error> decode_header(std::span<const std::byte> bytes,
                                                  const ArchiveLimits &limits) {
    if (bytes.size() != archive_header_bytes) {
        return std::unexpected(
            Error{ErrorCode::malformed_archive, "archive header must contain exactly 96 bytes"});
    }
    if (!std::ranges::equal(bytes.first(magic.size()), magic)) {
        return std::unexpected(
            Error{ErrorCode::malformed_archive, "invalid experimental archive magic"});
    }
    if (read_le<std::uint32_t>(bytes, 92) != crc32c(bytes.first(92))) {
        return std::unexpected(Error{ErrorCode::integrity_error, "archive header CRC32C mismatch"});
    }
    if (read_le<std::uint16_t>(bytes, 10) != archive_header_bytes ||
        read_le<std::uint32_t>(bytes, 84) != 0 || read_le<std::uint32_t>(bytes, 88) != 0) {
        return std::unexpected(
            Error{ErrorCode::malformed_archive, "invalid header size or nonzero reserved fields"});
    }

    ArchiveHeader header;
    header.revision = read_le<std::uint16_t>(bytes, 8);
    header.flags = read_le<std::uint32_t>(bytes, 12);
    header.strategy_version = read_le<std::uint32_t>(bytes, 16);
    header.seed_length = read_le<std::uint16_t>(bytes, 20);
    header.checksum_id = read_le<std::uint16_t>(bytes, 22);
    header.index_bytes = read_le<std::uint64_t>(bytes, 24);
    header.entry_count = read_le<std::uint64_t>(bytes, 32);
    header.block_count = read_le<std::uint64_t>(bytes, 40);
    header.data_bytes = read_le<std::uint64_t>(bytes, 48);
    header.total_output = read_le<std::uint64_t>(bytes, 56);
    std::ranges::copy(bytes.subspan(64, 16), header.seed.begin());
    header.chunk_size_bytes = read_le<std::uint32_t>(bytes, 80);
    if (auto validation = validate_header(header, limits); !validation) {
        return std::unexpected(validation.error());
    }
    return header;
}

std::expected<void, Error> validate_block_layout(const ArchiveHeader &header,
                                                 std::span<const BlockDescriptor> blocks,
                                                 const ArchiveLimits &limits) {
    if (auto validation = validate_header(header, limits); !validation) {
        return std::unexpected(validation.error());
    }
    if (blocks.size() != header.block_count) {
        return std::unexpected(
            Error{ErrorCode::malformed_archive, "block descriptor count does not match header"});
    }
    std::uint64_t next_offset = 0;
    std::uint64_t raw_total = 0;
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        const auto &block = blocks[i];
        const auto fail = [i](ErrorCode code, const char *message) -> std::expected<void, Error> {
            return std::unexpected(Error{code, "block " + std::to_string(i) + ": " + message});
        };
        if (block.codec_id != 0 || block.codec_config_id != 0) {
            return fail(ErrorCode::unsupported_feature, "unsupported codec or codec configuration");
        }
        if (block.raw_size == 0 || block.stored_size == 0 || block.stored_size != block.raw_size) {
            return fail(ErrorCode::malformed_archive, "invalid Store block lengths");
        }
        if (block.raw_size > header.chunk_size_bytes || block.raw_size > limits.max_chunk_bytes) {
            return fail(ErrorCode::resource_limit, "block exceeds chunk limit");
        }
        const auto end = checked_add(block.data_offset, block.stored_size);
        const auto updated_raw_total = checked_add(raw_total, block.raw_size);
        if (!end || !updated_raw_total) {
            return fail(ErrorCode::arithmetic_overflow, "block range or output size overflow");
        }
        if (block.data_offset != next_offset || *end > header.data_bytes) {
            return fail(ErrorCode::malformed_archive,
                        "block range overlaps, has a gap or is out of bounds");
        }
        if (*updated_raw_total > limits.max_total_output) {
            return fail(ErrorCode::resource_limit, "block output exceeds total limit");
        }
        next_offset = *end;
        raw_total = *updated_raw_total;
    }
    if (next_offset != header.data_bytes || raw_total != header.total_output) {
        return std::unexpected(
            Error{ErrorCode::malformed_archive, "block totals do not match archive header"});
    }
    return {};
}
} // namespace seedarc::experimental
