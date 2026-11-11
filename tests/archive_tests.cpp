// SPDX-License-Identifier: GPL-3.0-or-later
#include "seedarc/archive.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <string_view>

namespace {
using seedarc::ErrorCode;
using namespace seedarc::experimental;

int failures = 0;

void check(bool condition, const char *description) {
    if (!condition) {
        std::cerr << "FAIL: " << description << '\n';
        ++failures;
    }
}

template <typename T>
void check_error(const std::expected<T, seedarc::Error> &result, ErrorCode code,
                 const char *description) {
    check(!result && result.error().code == code && !result.error().message.empty(), description);
}

void set_le(std::span<std::byte> bytes, std::size_t offset, std::uint64_t value,
            std::size_t width) {
    for (std::size_t i = 0; i < width; ++i) {
        bytes[offset + i] = static_cast<std::byte>((value >> (i * 8)) & 0xffU);
    }
}

void refresh_crc(std::array<std::byte, archive_header_bytes> &bytes) {
    set_le(bytes, 92, crc32c(std::span{bytes}.first(92)), 4);
}

std::array<std::byte, archive_header_bytes> golden_header() {
    // Fixed wire vector; CRC derived independently using the MSB-first polynomial 0x1edc6f41.
    constexpr std::string_view hex =
        "5341524345585000010060000000000000000000000001008000000000000000"
        "0100000000000000020000000000000006000000000000000600000000000000"
        "000000000000000000000000000000000400000000000000000000001010c7ef";
    static_assert(hex.size() == archive_header_bytes * 2);
    const auto digit = [](char value) -> unsigned {
        return value >= '0' && value <= '9' ? static_cast<unsigned>(value - '0')
                                            : static_cast<unsigned>(value - 'a' + 10);
    };
    std::array<std::byte, archive_header_bytes> bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<std::byte>((digit(hex[2 * i]) << 4) | digit(hex[2 * i + 1]));
    }
    return bytes;
}

ArchiveHeader sample_header() {
    ArchiveHeader header;
    header.index_bytes = 128;
    header.entry_count = 1;
    header.block_count = 2;
    header.data_bytes = 6;
    header.total_output = 6;
    header.chunk_size_bytes = 4;
    return header;
}

ArchiveLimits unbounded_test_limits() {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    ArchiveLimits limits;
    limits.max_index_bytes = maximum;
    limits.max_entries = maximum;
    limits.max_blocks = maximum;
    limits.max_data_bytes = maximum;
    limits.max_total_output = maximum;
    limits.max_chunk_bytes = std::numeric_limits<std::uint32_t>::max();
    return limits;
}

void test_crc_and_wire_vector() {
    constexpr std::array text{std::byte{'1'}, std::byte{'2'}, std::byte{'3'},
                              std::byte{'4'}, std::byte{'5'}, std::byte{'6'},
                              std::byte{'7'}, std::byte{'8'}, std::byte{'9'}};
    check(crc32c(text) == 0xe3069283U, "CRC32C standard check vector");
    check(crc32c({}) == 0, "CRC32C empty input");

    const auto header = sample_header();
    const auto encoded = encode_header(header);
    check(encoded && *encoded == golden_header(), "fixed complete little-endian wire vector");
    const auto decoded = decode_header(golden_header());
    check(decoded && *decoded == header, "decode fixed wire vector");
    const auto empty_bytes = encode_header({});
    check(empty_bytes.has_value(), "encode canonical empty header");
    if (empty_bytes) {
        const auto empty = decode_header(*empty_bytes);
        check(empty && *empty == ArchiveHeader{}, "empty header round trip");
    }
}

void test_header_damage_and_features() {
    const auto golden = golden_header();
    for (std::size_t size = 0; size < golden.size(); ++size) {
        check_error(decode_header(std::span{golden}.first(size)), ErrorCode::malformed_archive,
                    "reject every truncated header prefix");
    }
    std::array<std::byte, archive_header_bytes + 1> extra{};
    std::ranges::copy(golden, extra.begin());
    check_error(decode_header(extra), ErrorCode::malformed_archive, "reject extra header byte");

    auto changed = golden;
    changed[0] = std::byte{0};
    check_error(decode_header(changed), ErrorCode::malformed_archive, "reject magic mismatch");
    changed = golden;
    changed[24] ^= std::byte{1};
    check_error(decode_header(changed), ErrorCode::integrity_error, "reject corrupted header body");
    changed = golden;
    changed[92] ^= std::byte{1};
    check_error(decode_header(changed), ErrorCode::integrity_error, "reject corrupted header CRC");

    const auto mutation = [&golden](std::size_t offset, std::uint64_t value, std::size_t width) {
        auto bytes = golden;
        set_le(bytes, offset, value, width);
        refresh_crc(bytes);
        return bytes;
    };
    check_error(decode_header(mutation(8, 2, 2)), ErrorCode::unsupported_feature,
                "reject unknown revision with valid CRC");
    check_error(decode_header(mutation(10, 95, 2)), ErrorCode::malformed_archive,
                "reject wrong declared header length");
    check_error(decode_header(mutation(12, 1, 4)), ErrorCode::unsupported_feature,
                "reject unknown flags");
    check_error(decode_header(mutation(16, 1, 4)), ErrorCode::unsupported_feature,
                "reject unimplemented strategy");
    check_error(decode_header(mutation(20, 16, 2)), ErrorCode::unsupported_feature,
                "recognize but reject unimplemented structured seed");
    check_error(decode_header(mutation(20, 8, 2)), ErrorCode::malformed_archive,
                "reject invalid seed length");
    check_error(decode_header(mutation(22, 2, 2)), ErrorCode::unsupported_feature,
                "reject unknown checksum");
    check_error(decode_header(mutation(64, 1, 1)), ErrorCode::malformed_archive,
                "reject nonzero unused seed byte");
    check_error(decode_header(mutation(80, 0, 4)), ErrorCode::malformed_archive,
                "reject zero chunk size");
    check_error(decode_header(mutation(84, 1, 4)), ErrorCode::malformed_archive,
                "reject first reserved field");
    check_error(decode_header(mutation(88, 1, 4)), ErrorCode::malformed_archive,
                "reject second reserved field");
    check_error(decode_header(mutation(24, 127, 8)), ErrorCode::malformed_archive,
                "reject insufficient index for record counts");
}

void test_header_limits_and_arithmetic() {
    const auto header = sample_header();
    auto limits = ArchiveLimits{};
    limits.max_index_bytes = 127;
    check_error(encode_header(header, limits), ErrorCode::resource_limit, "index limit on encode");
    check_error(decode_header(golden_header(), limits), ErrorCode::resource_limit,
                "index limit on decode");
    limits = {};
    limits.max_entries = 0;
    check_error(encode_header(header, limits), ErrorCode::resource_limit, "entry count limit");
    limits = {};
    limits.max_blocks = 1;
    check_error(encode_header(header, limits), ErrorCode::resource_limit, "block count limit");
    limits = {};
    limits.max_data_bytes = 5;
    check_error(encode_header(header, limits), ErrorCode::resource_limit, "stored data limit");
    limits = {};
    limits.max_total_output = 5;
    check_error(encode_header(header, limits), ErrorCode::resource_limit, "total output limit");
    limits = {};
    limits.max_chunk_bytes = 3;
    check_error(encode_header(header, limits), ErrorCode::resource_limit, "chunk size limit");

    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    auto changed = header;
    changed.entry_count = maximum;
    changed.index_bytes = maximum;
    check_error(encode_header(changed, unbounded_test_limits()), ErrorCode::arithmetic_overflow,
                "checked index entry size multiplication");
    changed = header;
    changed.block_count = maximum;
    changed.index_bytes = maximum;
    check_error(encode_header(changed, unbounded_test_limits()), ErrorCode::arithmetic_overflow,
                "checked block table size multiplication");
    changed = header;
    changed.entry_count = maximum / entry_record_min_bytes;
    changed.block_count = 1;
    changed.index_bytes = maximum;
    check_error(encode_header(changed, unbounded_test_limits()), ErrorCode::arithmetic_overflow,
                "checked index record size addition");
    changed = header;
    changed.index_bytes = maximum - 95;
    check_error(encode_header(changed, unbounded_test_limits()), ErrorCode::arithmetic_overflow,
                "checked header plus index overflow");
    changed = header;
    changed.index_bytes = maximum - 100;
    check_error(encode_header(changed, unbounded_test_limits()), ErrorCode::arithmetic_overflow,
                "checked archive total length overflow");

    changed = header;
    changed.entry_count = 0;
    check_error(encode_header(changed), ErrorCode::malformed_archive,
                "reject unowned archive data");
    changed = header;
    changed.block_count = 0;
    check_error(encode_header(changed), ErrorCode::malformed_archive, "reject data without blocks");
    changed = header;
    changed.flags = 1;
    check_error(encode_header(changed), ErrorCode::unsupported_feature,
                "encoder also rejects unsupported flags");
    changed = header;
    changed.seed_length = 16;
    check_error(encode_header(changed), ErrorCode::unsupported_feature,
                "encoder also rejects unimplemented seed strategies");
}

// Boundary values for each default limit: exactly at the limit is accepted (and round-trips),
// one past it is a resource_limit error. Also checks that all maxima hold at the same time.
void test_default_limit_boundaries() {
    const ArchiveLimits limits{};
    check(limits_are_coherent(limits), "default limits are coherent");
    check(!limits_are_coherent(unbounded_test_limits()), "unbounded test limits are incoherent");
    auto too_many_blocks = limits;
    too_many_blocks.max_blocks =
        (limits.max_index_bytes - entry_record_min_bytes) / block_record_bytes + 1;
    too_many_blocks.max_entries = 1;
    check(!limits_are_coherent(too_many_blocks), "unreachable block limit detected");

    const auto at_limits = [&](std::uint64_t entries, std::uint64_t blocks) {
        ArchiveHeader header;
        header.entry_count = entries;
        header.block_count = blocks;
        header.index_bytes = entries * entry_record_min_bytes + blocks * block_record_bytes;
        header.data_bytes = blocks; // one byte per block keeps data / output tiny
        header.total_output = blocks;
        return header;
    };
    const auto accepts = [&](const ArchiveHeader &header, const char *description) {
        const auto encoded = encode_header(header, limits);
        const auto decoded =
            encoded
                ? decode_header(*encoded, limits)
                : std::expected<ArchiveHeader, seedarc::Error>{std::unexpected(encoded.error())};
        check(decoded && *decoded == header, description);
    };
    accepts(at_limits(limits.max_entries, 1), "entry count exactly at limit");
    accepts(at_limits(1, limits.max_blocks), "block count exactly at limit");
    accepts(at_limits(limits.max_entries, limits.max_blocks), "entry and block maxima together");
    check_error(encode_header(at_limits(limits.max_entries + 1, 1), limits),
                ErrorCode::resource_limit, "entry count one past limit");
    check_error(encode_header(at_limits(1, limits.max_blocks + 1), limits),
                ErrorCode::resource_limit, "block count one past limit");

    auto header = at_limits(1, 1);
    header.index_bytes = limits.max_index_bytes;
    accepts(header, "index bytes exactly at limit");
    header.index_bytes = limits.max_index_bytes + 1;
    check_error(encode_header(header, limits), ErrorCode::resource_limit,
                "index bytes one past limit");

    header = at_limits(1, 1);
    header.data_bytes = limits.max_data_bytes;
    header.total_output = limits.max_total_output;
    header.chunk_size_bytes = limits.max_chunk_bytes;
    accepts(header, "data, output and chunk size exactly at limit");
    for (int field = 0; field < 3; ++field) {
        auto over = header;
        if (field == 0) {
            over.data_bytes += 1;
        } else if (field == 1) {
            over.total_output += 1;
        } else {
            over.chunk_size_bytes += 1;
        }
        check_error(encode_header(over, limits), ErrorCode::resource_limit,
                    "data, output or chunk size one past limit");
    }
}

void test_block_layout() {
    const auto header = sample_header();
    const std::array<BlockDescriptor, 2> blocks{{{0, 4, 4, 0, 0, 0}, {4, 2, 2, 0, 0, 0}}};
    check(validate_block_layout(header, blocks).has_value(), "valid contiguous Store layout");
    check(validate_block_layout({}, {}).has_value(), "empty layout");
    check_error(validate_block_layout(header, std::span{blocks}.first(1)),
                ErrorCode::malformed_archive, "reject block count mismatch");

    auto changed = blocks;
    changed[1].data_offset = 3;
    check_error(validate_block_layout(header, changed), ErrorCode::malformed_archive,
                "reject overlapping block");
    changed = blocks;
    changed[0].data_offset = 1;
    check_error(validate_block_layout(header, changed), ErrorCode::malformed_archive,
                "reject leading gap");
    changed = blocks;
    changed[1].data_offset = 5;
    check_error(validate_block_layout(header, changed), ErrorCode::malformed_archive,
                "reject block range past data bounds");
    changed = blocks;
    changed[1].data_offset = std::numeric_limits<std::uint64_t>::max();
    check_error(validate_block_layout(header, changed), ErrorCode::arithmetic_overflow,
                "reject block offset plus length overflow");
    changed = blocks;
    changed[0].codec_id = 1;
    check_error(validate_block_layout(header, changed), ErrorCode::unsupported_feature,
                "reject unimplemented codec");
    changed = blocks;
    changed[0].codec_config_id = 1;
    check_error(validate_block_layout(header, changed), ErrorCode::unsupported_feature,
                "reject unknown codec configuration");
    changed = blocks;
    changed[0].raw_size = 0;
    changed[0].stored_size = 0;
    check_error(validate_block_layout(header, changed), ErrorCode::malformed_archive,
                "reject zero-size block");
    changed = blocks;
    changed[0].stored_size = 3;
    check_error(validate_block_layout(header, changed), ErrorCode::malformed_archive,
                "reject Store length mismatch");
    changed = blocks;
    changed[0].raw_size = 5;
    changed[0].stored_size = 5;
    check_error(validate_block_layout(header, changed), ErrorCode::resource_limit,
                "reject block larger than selected chunk size");

    auto changed_header = header;
    changed_header.data_bytes = 7;
    check_error(validate_block_layout(changed_header, blocks), ErrorCode::malformed_archive,
                "reject unreferenced trailing data");
    changed_header = header;
    changed_header.total_output = 5;
    check_error(validate_block_layout(changed_header, blocks), ErrorCode::malformed_archive,
                "reject wrong aggregate raw length");
    changed_header = {};
    changed_header.entry_count = 1;
    changed_header.index_bytes = entry_record_min_bytes;
    check(validate_block_layout(changed_header, {}).has_value(),
          "empty-file metadata needs no blocks");
}
} // namespace

int main() {
    test_crc_and_wire_vector();
    test_header_damage_and_features();
    test_header_limits_and_arithmetic();
    test_default_limit_boundaries();
    test_block_layout();
    return failures == 0 ? 0 : 1;
}
