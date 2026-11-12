// SPDX-License-Identifier: GPL-3.0-or-later
#include "seedarc/archive_index.hpp"

#include <array>
#include <functional>
#include <iostream>
#include <string>
#include <string_view>

namespace {
using seedarc::ErrorCode;
using namespace seedarc::experimental;

int failures = 0;

void check(bool condition, std::string_view description) {
    if (!condition) {
        std::cerr << "FAIL: " << description << '\n';
        ++failures;
    }
}

void check_error(const std::expected<ArchiveIndex, seedarc::Error> &result, ErrorCode code,
                 std::string_view fragment, std::string_view description) {
    const bool matches = !result && result.error().code == code &&
                         result.error().message.find(fragment) != std::string::npos;
    if (!matches) {
        std::cerr << "FAIL: " << description << " (got "
                  << (result ? "success" : result.error().message) << ")\n";
        ++failures;
    }
}

struct Sample {
    ArchiveHeader header;
    ArchiveIndex index;
};

// a/ (dir), a/b.bin (6 bytes, 2 blocks), a/empty (0 bytes), c.txt (4 bytes, 1 block), d/ (dir)
Sample sample() {
    Sample result;
    result.index.entries = {{"a", true, 0, 0, 0},
                            {"a/b.bin", false, 6, 0, 2},
                            {"a/empty", false, 0, 2, 0},
                            {"c.txt", false, 4, 2, 1},
                            {"d", true, 0, 3, 0}};
    result.index.blocks = {
        {0, 4, 4, 0, 0, 0x01020304U}, {4, 2, 2, 0, 0, 0x05060708U}, {6, 4, 4, 0, 0, 0x090a0b0cU}};
    result.header.chunk_size_bytes = 4;
    return result;
}

// Serializes the sample and makes the header describe exactly what was serialized.
std::vector<std::byte> seal(Sample &value) {
    auto bytes = encode_index(value.index);
    value.header.entry_count = value.index.entries.size();
    value.header.block_count = value.index.blocks.size();
    value.header.index_bytes = bytes.size();
    std::uint64_t data = 0;
    for (const auto &block : value.index.blocks) {
        data += block.stored_size;
    }
    std::uint64_t output = 0;
    for (const auto &entry : value.index.entries) {
        output += entry.size;
    }
    value.header.data_bytes = data;
    value.header.total_output = output;
    return bytes;
}

void refresh_crc(std::vector<std::byte> &bytes) {
    const auto body = bytes.size() - index_trailer_bytes;
    const auto crc = crc32c(std::span{bytes}.first(body));
    for (std::size_t i = 0; i < 4; ++i) {
        bytes[body + i] = static_cast<std::byte>((crc >> (i * 8)) & 0xffU);
    }
}

// Applies `change` to a fresh sample and expects parse_index to reject the result.
void reject(const std::function<void(Sample &)> &change, ErrorCode code, std::string_view fragment,
            std::string_view description) {
    auto value = sample();
    change(value);
    const auto bytes = seal(value);
    check_error(parse_index(value.header, bytes), code, fragment, description);
}

// Patches raw index bytes of the valid sample (with a refreshed CRC) and expects rejection.
void reject_patch(const std::function<void(std::vector<std::byte> &)> &patch, ErrorCode code,
                  std::string_view fragment, std::string_view description) {
    auto value = sample();
    auto bytes = seal(value);
    patch(bytes);
    refresh_crc(bytes);
    check_error(parse_index(value.header, bytes), code, fragment, description);
}

void test_order() {
    check(archive_path_less("a", "a/b"), "directory sorts before its children");
    check(archive_path_less("a/b", "a.txt"), "subtree sorts before a sibling with a suffix");
    check(!archive_path_less("a.txt", "a/b"), "order is strict");
    check(!archive_path_less("a", "a"), "equal paths are not less");
    check(archive_path_less("a", "b") && !archive_path_less("b", "a"), "byte order");
    check(archive_path_less("B", "a"), "uppercase sorts before lowercase (byte order)");
    check(archive_path_less("z", "\xc3\xa9"), "non-ASCII bytes compare as unsigned");
    check(archive_path_less("a/b/c", "a/c"), "component-wise comparison");
}

void test_round_trip_and_wire_vector() {
    auto value = sample();
    const auto bytes = seal(value);
    const auto size = encoded_index_bytes(value.index);
    check(size && *size == bytes.size(), "encoded_index_bytes matches encode_index");
    check(bytes.size() == 5 * 32 + 1 + 7 + 7 + 5 + 1 + 3 * 48 + 4, "index size formula");
    const auto parsed = parse_index(value.header, bytes);
    check(parsed && *parsed == value.index, "valid index round trip");
    check(parsed && encode_index(*parsed) == bytes, "accepted index re-encodes identically");

    // Fixed wire vector for one file entry "a" (1 byte, 1 block) with raw CRC 0x11223344.
    ArchiveIndex one;
    one.entries = {{"a", false, 1, 0, 1}};
    one.blocks = {{0, 1, 1, 0, 0, 0x11223344U}};
    constexpr std::array<unsigned char, 81> expected{
        1, 0, 0, 0, 0, 0, 0, 0, 1,    0,    0,    0,    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        1, 0, 0, 0, 0, 0, 0, 0, 'a', // entry record and path
        0, 0, 0, 0, 0, 0, 0, 0, 1,    0,    0,    0,    0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0x44, 0x33, 0x22, 0x11, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    const auto wire = encode_index(one);
    check(wire.size() == expected.size() + index_trailer_bytes, "wire vector size");
    bool same = wire.size() == expected.size() + index_trailer_bytes;
    for (std::size_t i = 0; same && i < expected.size(); ++i) {
        same = wire[i] == static_cast<std::byte>(expected[i]);
    }
    check(same, "fixed little-endian index wire vector");
    if (same) {
        const auto crc = crc32c(std::span{wire}.first(expected.size()));
        for (std::size_t i = 0; i < 4; ++i) {
            same = same &&
                   wire[expected.size() + i] == static_cast<std::byte>((crc >> (i * 8)) & 0xffU);
        }
        check(same, "index trailer is the CRC32C of the records");
    }

    check(encode_index({}).empty(), "empty index encodes to nothing");
    const auto empty = parse_index({}, {});
    check(empty && empty->entries.empty() && empty->blocks.empty(), "empty index accepted");
    const std::array<std::byte, 4> stray{};
    check_error(parse_index({}, stray), ErrorCode::malformed_archive, "length",
                "empty archive with index bytes rejected");
}

void test_damage() {
    auto value = sample();
    const auto bytes = seal(value);
    check_error(parse_index(value.header, std::span{bytes}.first(bytes.size() - 1)),
                ErrorCode::malformed_archive, "length", "truncated index rejected");
    for (const std::size_t position : {std::size_t{0}, std::size_t{40}, bytes.size() - 1}) {
        auto changed = bytes;
        changed[position] ^= std::byte{0x10};
        check_error(parse_index(value.header, changed), ErrorCode::integrity_error, "CRC32C",
                    "flipped index byte rejected");
    }
    {
        // A header that claims a shorter index over the same bytes.
        auto shorter = value.header;
        shorter.index_bytes -= 1;
        check_error(parse_index(shorter, bytes), ErrorCode::malformed_archive, "length",
                    "index longer than declared rejected");
    }

    reject([](Sample &s) { std::swap(s.index.entries[3], s.index.entries[4]); },
           ErrorCode::malformed_archive, "", "entries out of order rejected");
    reject(
        [](Sample &s) {
            s.index.entries = {{"b", true, 0, 0, 0}, {"a", true, 0, 0, 0}};
            s.index.blocks.clear();
        },
        ErrorCode::malformed_archive, "canonical order", "unsorted directories rejected");
    reject(
        [](Sample &s) {
            s.index.entries = {{"a", true, 0, 0, 0}, {"a", true, 0, 0, 0}};
            s.index.blocks.clear();
        },
        ErrorCode::malformed_archive, "duplicate path", "duplicate path rejected");
    reject(
        [](Sample &s) {
            s.index.entries = {{"x/y", true, 0, 0, 0}};
            s.index.blocks.clear();
        },
        ErrorCode::malformed_archive, "parent directory", "missing parent rejected");
    reject(
        [](Sample &s) {
            s.index.entries = {{"x", false, 0, 0, 0}, {"x/y", false, 0, 0, 0}};
            s.index.blocks.clear();
        },
        ErrorCode::unsafe_path, "", "file used as a directory rejected");
    reject(
        [](Sample &s) {
            s.index.entries = {{"A", true, 0, 0, 0}, {"a", true, 0, 0, 0}};
            s.index.blocks.clear();
        },
        ErrorCode::unsafe_path, "case-colliding", "case collision rejected");
    for (const char *path : {"../evil", "..", "a/../b", "/abs", "C:evil", "a\\b", "a//b", "nul",
                             "trailing.", "a/./b"}) {
        reject(
            [path](Sample &s) {
                s.index.entries = {{path, false, 0, 0, 0}};
                s.index.blocks.clear();
            },
            ErrorCode::unsafe_path, "", std::string{"unsafe path rejected: "} + path);
    }
    reject(
        [](Sample &s) {
            s.index.entries[1].block_count = 1;
            s.index.entries[2].first_block = 1;
            s.index.entries[3].first_block = 1;
        },
        ErrorCode::malformed_archive, "block count does not match", "wrong block count");
    reject([](Sample &s) { s.index.entries[1].first_block = 1; }, ErrorCode::malformed_archive,
           "dangling", "dangling first block rejected");
    reject([](Sample &s) { s.index.entries[3].first_block = 1; }, ErrorCode::malformed_archive,
           "overlapping", "block shared by two files rejected");
    reject([](Sample &s) { s.index.blocks.push_back({10, 4, 4, 0, 0, 0}); },
           ErrorCode::malformed_archive, "account for every block", "unowned block rejected");
    reject([](Sample &s) { s.index.blocks.pop_back(); }, ErrorCode::malformed_archive, "dangling",
           "reference beyond the block table rejected");
    reject([](Sample &s) { s.index.blocks[1].data_offset = 0; }, ErrorCode::malformed_archive,
           "overlaps", "overlapping block ranges rejected");
    reject([](Sample &s) { s.index.blocks[1].data_offset = 5; }, ErrorCode::malformed_archive,
           "gap", "undeclared data region rejected");
    reject(
        [](Sample &s) {
            s.index.blocks[0].stored_size = s.index.blocks[0].raw_size = 2;
            s.index.blocks[1].data_offset = 2;
            s.index.blocks[1].stored_size = s.index.blocks[1].raw_size = 4;
        },
        ErrorCode::malformed_archive, "fixed chunking", "short non-final block rejected");
    reject([](Sample &s) { s.index.blocks[0].stored_size = 3; }, ErrorCode::malformed_archive,
           "invalid block lengths", "stored and raw length mismatch rejected");
    reject([](Sample &s) { s.index.blocks[0].codec_id = 1; }, ErrorCode::unsupported_feature,
           "codec", "unknown codec rejected");
    // Codec / configuration pairs (ADR-0003): the last block (c.txt, 4 raw bytes) is changed so
    // that no later offsets move.
    const auto codec_block = [](std::uint32_t codec, std::uint32_t config, std::uint64_t stored) {
        return [=](Sample &s) {
            auto &block = s.index.blocks[2];
            block.codec_id = codec;
            block.codec_config_id = config;
            block.stored_size = stored;
        };
    };
    reject(codec_block(0, 1, 4), ErrorCode::unsupported_feature, "codec",
           "Store with a configuration rejected");
    reject(codec_block(1, 0, 3), ErrorCode::unsupported_feature, "codec", "zstd level 0 rejected");
    reject(codec_block(1, 20, 3), ErrorCode::unsupported_feature, "codec",
           "zstd level 20 rejected");
    reject(codec_block(2, 10, 3), ErrorCode::unsupported_feature, "codec",
           "LZMA2 preset 10 rejected");
    reject(codec_block(3, 0, 3), ErrorCode::unsupported_feature, "codec", "codec 3 rejected");
    reject(codec_block(1, 3, 4), ErrorCode::malformed_archive, "invalid block lengths",
           "zstd block not smaller than raw rejected");
    reject(codec_block(2, 6, 5), ErrorCode::malformed_archive, "invalid block lengths",
           "LZMA2 block larger than raw rejected");
    for (const auto &[codec, config] : std::array<std::pair<std::uint32_t, std::uint32_t>, 4>{
             {{1, 1}, {1, 19}, {2, 0}, {2, 9}}}) {
        auto accepted = sample();
        codec_block(codec, config, 3)(accepted);
        const auto sealed = seal(accepted);
        const auto parsed = parse_index(accepted.header, sealed);
        check(parsed.has_value() && parsed->blocks[2] == accepted.index.blocks[2],
              "smaller compressed block with codec " + std::to_string(codec) + " config " +
                  std::to_string(config) + " accepted");
    }
    reject([](Sample &s) { s.index.entries[0].size = 1; }, ErrorCode::malformed_archive,
           "directory declares data", "directory with data rejected");
    {
        auto changed = sample();
        const auto sealed = seal(changed);
        changed.header.total_output += 1;
        check_error(parse_index(changed.header, sealed), ErrorCode::malformed_archive, "",
                    "total output mismatch rejected");
        changed.header.total_output -= 1;
        changed.header.block_count += 1;
        check_error(parse_index(changed.header, sealed), ErrorCode::malformed_archive, "",
                    "block count mismatch rejected");
        changed.header.block_count -= 1;
        changed.header.entry_count -= 1;
        check_error(parse_index(changed.header, sealed), ErrorCode::malformed_archive, "",
                    "entry count mismatch rejected");
    }

    reject_patch([](std::vector<std::byte> &b) { b[4] = std::byte{2}; },
                 ErrorCode::malformed_archive, "unknown entry kind", "unknown kind rejected");
    reject_patch([](std::vector<std::byte> &b) { b[6] = std::byte{1}; },
                 ErrorCode::malformed_archive, "reserved", "entry reserved bytes rejected");
    reject_patch([](std::vector<std::byte> &b) { b[0] = std::byte{0}; },
                 ErrorCode::malformed_archive, "path length", "zero path length rejected");
    reject_patch([](std::vector<std::byte> &b) { b[2] = std::byte{0xff}; },
                 ErrorCode::malformed_archive, "path length", "path beyond the index rejected");
    reject_patch([](std::vector<std::byte> &b) { b[b.size() - 5] = std::byte{1}; },
                 ErrorCode::malformed_archive, "reserved", "block reserved bytes rejected");
    // Offset 180 is the path byte of the last entry ("d"): 33 + 39 + 39 + 37 + 32.
    reject_patch([](std::vector<std::byte> &b) { b[180] = std::byte{0xff}; },
                 ErrorCode::unsafe_path, "UTF-8", "invalid UTF-8 path rejected");
}

void test_limits() {
    auto value = sample();
    const auto bytes = seal(value);
    const auto with = [&](const std::function<void(ArchiveLimits &)> &change) {
        ArchiveLimits limits;
        change(limits);
        return parse_index(value.header, bytes, limits);
    };
    check(with([](ArchiveLimits &l) { l.max_entries = 5; }).has_value(), "entries at limit");
    check_error(with([](ArchiveLimits &l) { l.max_entries = 4; }), ErrorCode::resource_limit, "",
                "entries over limit");
    check(with([](ArchiveLimits &l) { l.max_blocks = 3; }).has_value(), "blocks at limit");
    check_error(with([](ArchiveLimits &l) { l.max_blocks = 2; }), ErrorCode::resource_limit, "",
                "blocks over limit");
    check(with([&](ArchiveLimits &l) { l.max_index_bytes = bytes.size(); }).has_value(),
          "index bytes at limit");
    check_error(with([&](ArchiveLimits &l) { l.max_index_bytes = bytes.size() - 1; }),
                ErrorCode::resource_limit, "", "index bytes over limit");
    check(with([](ArchiveLimits &l) { l.max_total_output = 10; }).has_value(), "output at limit");
    check_error(with([](ArchiveLimits &l) { l.max_total_output = 9; }), ErrorCode::resource_limit,
                "", "output over limit");
    check(with([](ArchiveLimits &l) { l.max_data_bytes = 10; }).has_value(), "data at limit");
    check_error(with([](ArchiveLimits &l) { l.max_data_bytes = 9; }), ErrorCode::resource_limit, "",
                "data over limit");
    check(with([](ArchiveLimits &l) { l.max_chunk_bytes = 4; }).has_value(), "chunk at limit");
    check_error(with([](ArchiveLimits &l) { l.max_chunk_bytes = 3; }), ErrorCode::resource_limit,
                "", "chunk over limit");
}
} // namespace

int main() {
    test_order();
    test_round_trip_and_wire_vector();
    test_damage();
    test_limits();
    return failures == 0 ? 0 : 1;
}
