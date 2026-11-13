// SPDX-License-Identifier: GPL-3.0-or-later
// M3 codec tests: the codec boundary on its own (hostile and truncated streams, parameters, size
// mismatches) and Zstandard / LZMA2 archives end to end (round trips, Store fallback, determinism,
// corrupted data). Usage: <sandbox directory>.
#include "codec/codec.hpp"
#include "seedarc/archive_index.hpp"
#include "seedarc/store_archive.hpp"
#include "test_fixture.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace fs = std::filesystem;
using seedarc::ErrorCode;
using namespace seedarc::experimental;

namespace {
int failures = 0;

void check(bool condition, const std::string &description) {
    if (!condition) {
        std::cerr << "FAIL: " << description << '\n';
        ++failures;
    }
}

template <class T>
void check_code(const std::expected<T, seedarc::Error> &result, ErrorCode code,
                const std::string &description) {
    const bool matches = !result && result.error().code == code;
    if (!matches) {
        std::cerr << "FAIL: " << description << " (got "
                  << (result ? std::string{"success"} : result.error().message) << ")\n";
        ++failures;
    }
}

// Deterministic test data (SplitMix64); `alphabet` < 256 makes it compressible.
std::vector<std::byte> make_data(std::size_t size, std::uint64_t seed, unsigned alphabet = 256) {
    std::vector<std::byte> bytes(size);
    for (auto &value : bytes) {
        seed += 0x9e3779b97f4a7c15ULL;
        auto z = seed;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        z ^= z >> 31;
        value = static_cast<std::byte>(z % alphabet);
    }
    return bytes;
}

std::vector<std::byte> text_data(std::size_t size) {
    constexpr std::string_view line = "SeedArc compresses fixed chunks; every block is checked.\n";
    std::vector<std::byte> bytes(size);
    for (std::size_t i = 0; i < size; ++i) {
        bytes[i] = static_cast<std::byte>(line[i % line.size()]);
    }
    return bytes;
}

std::vector<std::byte> read_all(const fs::path &path) {
    std::ifstream stream(path, std::ios::binary);
    std::vector<char> chars{std::istreambuf_iterator<char>{stream}, {}};
    std::vector<std::byte> bytes(chars.size());
    std::ranges::transform(chars, bytes.begin(), [](char c) { return static_cast<std::byte>(c); });
    return bytes;
}

void write_file(const fs::path &path, std::span<const std::byte> bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char *>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    if (!stream) {
        throw std::runtime_error("cannot write test fixture");
    }
}

// Relative path -> content (directories map to nullopt).
using Snapshot = std::map<std::string, std::optional<std::vector<std::byte>>>;
Snapshot snapshot(const fs::path &root) {
    Snapshot result;
    for (const auto &item : fs::recursive_directory_iterator(root)) {
        const auto key = fs::relative(item.path(), root).generic_string();
        result[key] = item.is_directory() ? std::nullopt : std::optional{read_all(item.path())};
    }
    return result;
}

bool no_leftovers(const fs::path &directory) {
    for (const auto &child : fs::directory_iterator(directory)) {
        if (child.path().filename().wstring().starts_with(L".seedarc-")) {
            return false;
        }
    }
    return true;
}

// Codec ids of every block of an archive, read through the library's own validating parser.
std::optional<std::vector<BlockDescriptor>> blocks_of(const fs::path &archive) {
    const auto bytes = read_all(archive);
    if (bytes.size() < archive_header_bytes) {
        return std::nullopt;
    }
    const auto header = decode_header(std::span{bytes}.first(archive_header_bytes));
    if (!header) {
        return std::nullopt;
    }
    const auto index = parse_index(*header, std::span{bytes}.subspan(static_cast<std::size_t>(
                                                archive_header_bytes + header->data_bytes)));
    if (!index) {
        return std::nullopt;
    }
    return index->blocks;
}

// --- the codec boundary ------------------------------------------------------------------------

std::vector<std::byte> encoded(std::uint32_t id, std::uint32_t config,
                               std::span<const std::byte> input) {
    auto encoder = codec::Encoder::create(id, config);
    if (!encoder) {
        return {};
    }
    std::vector<std::byte> output(input.size() - 1);
    const auto size = encoder->encode(input, output);
    output.resize(size ? *size : 0);
    return output;
}

std::vector<std::byte> bytes_of(std::initializer_list<unsigned> values) {
    std::vector<std::byte> bytes;
    for (const auto value : values) {
        bytes.push_back(static_cast<std::byte>(value));
    }
    return bytes;
}

void codec_boundary_tests() {
    check(codec::library_versions().find("zstd 1.5.7") != std::string::npos &&
              codec::library_versions().find("liblzma 5.8.4") != std::string::npos,
          "linked library versions match the vcpkg manifest");
    for (const auto &[id, config] : std::array<std::pair<std::uint32_t, std::uint32_t>, 6>{
             {{0, 0}, {1, 0}, {1, 20}, {2, 10}, {3, 0}, {0, 1}}}) {
        check_code(codec::Encoder::create(id, config), ErrorCode::invalid_argument,
                   "encoder rejects codec " + std::to_string(id) + " config " +
                       std::to_string(config));
    }
    const auto input = text_data(100'000);
    codec::Decoder decoder;
    for (const auto &[id, config, name] :
         std::array<std::tuple<std::uint32_t, std::uint32_t, std::string_view>, 4>{
             {{codec::zstd_id, 1, "zstd 1"},
              {codec::zstd_id, 19, "zstd 19"},
              {codec::lzma2_id, 0, "lzma2 0"},
              {codec::lzma2_id, 9, "lzma2 9"}}}) {
        const std::string label{name};
        const auto packed = encoded(id, config, input);
        check(!packed.empty() && packed.size() < input.size() / 10, label + ": compresses text");
        std::vector<std::byte> output(input.size());
        check(decoder.decode(id, packed, output).has_value() && output == input,
              label + ": decodes exactly");
        check(packed == encoded(id, config, input), label + ": encoding is deterministic");

        // Truncation, trailing bytes and output size mismatches all fail as corrupt data.
        auto truncated = packed;
        truncated.pop_back();
        check_code(decoder.decode(id, truncated, output), ErrorCode::integrity_error,
                   label + ": truncated stream rejected");
        auto trailing = packed;
        trailing.push_back(std::byte{0});
        check_code(decoder.decode(id, trailing, output), ErrorCode::integrity_error,
                   label + ": trailing byte rejected");
        std::vector<std::byte> shorter(input.size() - 1);
        check_code(decoder.decode(id, packed, shorter), ErrorCode::integrity_error,
                   label + ": output shorter than the stream rejected");
        std::vector<std::byte> longer(input.size() + 1);
        check_code(decoder.decode(id, packed, longer), ErrorCode::integrity_error,
                   label + ": output longer than the stream rejected");
        check_code(decoder.decode(id, {}, output), ErrorCode::integrity_error,
                   label + ": empty input rejected");
        auto flipped = packed;
        flipped[flipped.size() / 2] ^= std::byte{0x5a};
        const auto damaged = decoder.decode(id, flipped, output);
        // A flipped byte may still decode to the right length; the block CRC32C then catches it.
        check(damaged ? output != input : damaged.error().code == ErrorCode::integrity_error,
              label + ": damaged stream fails or decodes to different bytes");
    }
    // Incompressible input never yields a "compressed" form that is not smaller.
    const auto noise = make_data(4096, 7);
    for (const auto id : {codec::zstd_id, codec::lzma2_id}) {
        check(encoded(id, id == codec::zstd_id ? 3 : 6, noise).empty(),
              "codec " + std::to_string(id) + ": random data reports not smaller");
    }
    auto encoder = codec::Encoder::create(codec::zstd_id, 3);
    std::vector<std::byte> none;
    const auto empty_capacity =
        encoder ? encoder->encode(input, none) : std::expected<std::size_t, seedarc::Error>{1};
    check(empty_capacity && *empty_capacity == 0, "zero output capacity reports not smaller");
    std::vector<std::byte> same(input.size());
    check_code(encoder->encode(input, same), ErrorCode::invalid_argument,
               "output capacity must be smaller than the input");

    // Hand-made Zstandard frames (RFC 8878): magic, frame header, one raw block of 300 bytes.
    const auto raw_block = [](std::vector<std::byte> frame) {
        constexpr auto header = (300U << 3) | 1U; // last block, raw, 300 bytes
        for (const auto b : bytes_of({header & 0xffU, (header >> 8) & 0xffU, header >> 16})) {
            frame.push_back(b);
        }
        frame.resize(frame.size() + 300, std::byte{'x'});
        return frame;
    };
    std::vector<std::byte> output(300);
    // Single segment, 2-byte content size 300 (stored as 300 - 256): valid.
    const auto valid = raw_block(bytes_of({0x28, 0xb5, 0x2f, 0xfd, 0x60, 44, 0}));
    check(decoder.decode(codec::zstd_id, valid, output).has_value() &&
              std::ranges::all_of(output, [](std::byte b) { return b == std::byte{'x'}; }),
          "hand-made zstd frame decodes");
    // No content size: rejected even though the data itself is fine.
    check_code(
        decoder.decode(codec::zstd_id, raw_block(bytes_of({0x28, 0xb5, 0x2f, 0xfd, 0, 0})), output),
        ErrorCode::integrity_error, "zstd frame without a content size rejected");
    // Declared window of 1 GiB for a 300-byte block: refused before decoding.
    check_code(decoder.decode(codec::zstd_id,
                              raw_block(bytes_of({0x28, 0xb5, 0x2f, 0xfd, 0x40, 0xa0, 44, 0})),
                              output),
               ErrorCode::resource_limit, "zstd window larger than the block rejected");
    // Two frames back to back.
    auto twice = valid;
    twice.insert(twice.end(), valid.begin(), valid.end());
    std::vector<std::byte> double_output(600);
    check_code(decoder.decode(codec::zstd_id, twice, double_output), ErrorCode::integrity_error,
               "two concatenated zstd frames rejected");
    // A skippable frame carries no content.
    auto skippable = bytes_of({0x50, 0x2a, 0x4d, 0x18, 4, 0, 0, 0, 1, 2, 3, 4});
    check_code(decoder.decode(codec::zstd_id, skippable, output), ErrorCode::integrity_error,
               "zstd skippable frame rejected");
    // Raw LZMA2: one uncompressed chunk (0x01, size - 1 big-endian) without the end marker.
    auto lzma = bytes_of({0x01, 0x01, 0x2b});
    lzma.resize(lzma.size() + 300, std::byte{'y'});
    check_code(decoder.decode(codec::lzma2_id, lzma, output), ErrorCode::integrity_error,
               "LZMA2 stream without end marker rejected");
    lzma.push_back(std::byte{0});
    check(decoder.decode(codec::lzma2_id, lzma, output).has_value(),
          "LZMA2 stream with end marker decodes");
    check_code(decoder.decode(9, valid, output), ErrorCode::unsupported_feature,
               "unknown codec id rejected by the decoder");
}

// --- archives ----------------------------------------------------------------------------------

struct Variant {
    std::string name;
    Codec codec;
    std::optional<std::uint32_t> level;
};

void archive_tests(const fs::path &work) {
    // Compressible text across several chunks, an exact chunk multiple, random data, a file
    // mixing both, an empty file, an empty directory and a non-ASCII name.
    const auto input = work / "input";
    fs::create_directories(input / "sub" / "empty");
    write_file(input / "text.txt", text_data(10'000));
    write_file(input / "exact.txt", text_data(4096));
    write_file(input / "random.bin", make_data(5000, 1));
    auto mixed = text_data(3000);
    const auto noise = make_data(3000, 2);
    mixed.insert(mixed.end(), noise.begin(), noise.end());
    write_file(input / "sub" / "mixed.bin", mixed);
    write_file(input / "sub" / "zero", {});
    write_file(input / fs::path{L"sub/été.txt"}, make_data(2000, 3, 4));
    const auto expected = snapshot(input);

    const std::vector<Variant> variants{{"store", Codec::store, std::nullopt},
                                        {"zstd", Codec::zstd, std::nullopt},
                                        {"zstd-1", Codec::zstd, 1},
                                        {"zstd-19", Codec::zstd, 19},
                                        {"lzma2", Codec::lzma2, std::nullopt},
                                        {"lzma2-0", Codec::lzma2, 0},
                                        {"lzma2-9", Codec::lzma2, 9}};
    std::uint64_t store_size = 0;
    for (const auto &variant : variants) {
        CompressOptions options;
        options.chunk_size_bytes = 1024;
        options.codec = variant.codec;
        options.level = variant.level;
        const auto archive = work / (variant.name + ".sarc");
        const auto compressed = compress_archive(input, archive, options);
        if (!compressed) {
            check(false, variant.name + ": compress (" + compressed.error().message + ")");
            continue;
        }
        check(fs::file_size(archive) == compressed->archive_bytes,
              variant.name + ": archive size reported");
        const auto verified = verify_archive(archive);
        check(verified.has_value() && verified->total_output == compressed->total_output,
              variant.name + ": verify");
        const auto output = work / (variant.name + "-out");
        check(extract_archive(archive, output).has_value() && snapshot(output) == expected,
              variant.name + ": byte-exact round trip");

        // The index records what was actually done per block: compressible chunks use the
        // codec, random chunks fall back to Store.
        const auto blocks = blocks_of(archive);
        const auto id = static_cast<std::uint32_t>(variant.codec);
        std::size_t compressed_blocks = 0;
        std::size_t stored_blocks = 0;
        bool consistent = blocks.has_value();
        for (const auto &block : blocks.value_or(std::vector<BlockDescriptor>{})) {
            if (block.codec_id == codec::store_id) {
                ++stored_blocks;
                consistent = consistent && block.codec_config_id == 0;
            } else {
                ++compressed_blocks;
                consistent = consistent && block.codec_id == id &&
                             block.codec_config_id ==
                                 variant.level.value_or(variant.codec == Codec::zstd ? 3U : 6U);
            }
        }
        check(consistent, variant.name + ": block codecs and levels recorded");
        if (variant.codec == Codec::store) {
            store_size = compressed->archive_bytes;
            check(compressed_blocks == 0, "store: no compressed blocks");
        } else {
            check(compressed_blocks > 0 && stored_blocks >= 5,
                  variant.name + ": text compressed and random chunks stored");
            check(compressed->archive_bytes < store_size,
                  variant.name + ": archive smaller than Store");
            // Identical input and options give an identical archive.
            const auto again = work / (variant.name + "-again.sarc");
            check(compress_archive(input, again, options).has_value() &&
                      read_all(again) == read_all(archive),
                  variant.name + ": archive bytes are reproducible");
        }
    }

    // Invalid options fail before anything is written.
    const auto rejected = [&](const std::string &label, Codec codec,
                              std::optional<std::uint32_t> level) {
        CompressOptions options;
        options.codec = codec;
        options.level = level;
        const auto archive = work / "rejected.sarc";
        check_code(compress_archive(input, archive, options), ErrorCode::invalid_argument, label);
        check(!fs::exists(archive) && no_leftovers(work), label + ": nothing written");
    };
    rejected("Store with a level rejected", Codec::store, 1);
    rejected("zstd level 0 rejected", Codec::zstd, 0);
    rejected("zstd level 20 rejected", Codec::zstd, 20);
    rejected("LZMA2 preset 10 rejected", Codec::lzma2, 10);
    rejected("unknown codec rejected", static_cast<Codec>(7), std::nullopt);

    // One-byte chunks can never shrink: everything is stored, and the result still round trips.
    {
        CompressOptions options;
        options.chunk_size_bytes = 1;
        options.codec = Codec::zstd;
        const auto small_input = work / "small";
        fs::create_directory(small_input);
        write_file(small_input / "a", text_data(64));
        const auto archive = work / "one-byte.sarc";
        const auto blocks = compress_archive(small_input, archive, options).has_value()
                                ? blocks_of(archive)
                                : std::nullopt;
        check(blocks && blocks->size() == 64 &&
                  std::ranges::all_of(*blocks,
                                      [](const auto &b) { return b.codec_id == codec::store_id; }),
              "one-byte chunks are all stored");
        check(extract_archive(archive, work / "one-byte-out").has_value() &&
                  read_all(work / "one-byte-out" / "a") == text_data(64),
              "one-byte chunk archive round trips");
    }

    // Damaged compressed data: verify and extract fail, nothing is published or left behind.
    for (const auto &name : {std::string{"zstd"}, std::string{"lzma2"}}) {
        const auto original = read_all(work / (name + ".sarc"));
        const auto blocks = blocks_of(work / (name + ".sarc"));
        const auto first =
            blocks ? std::ranges::find_if(
                         *blocks, [](const auto &b) { return b.codec_id != codec::store_id; })
                   : std::vector<BlockDescriptor>::const_iterator{};
        if (!blocks || first == blocks->end()) {
            check(false, name + ": damaged-data fixture has a compressed block");
            continue;
        }
        for (const auto &[label, position] :
             std::array<std::pair<std::string_view, std::uint64_t>, 2>{
                 {{"first byte", first->data_offset},
                  {"middle byte", first->data_offset + first->stored_size / 2}}}) {
            auto damaged = original;
            damaged[static_cast<std::size_t>(archive_header_bytes + position)] ^= std::byte{0x40};
            const auto archive = work / (name + "-damaged.sarc");
            fs::remove(archive);
            write_file(archive, damaged);
            const std::string what = name + " " + std::string{label};
            check_code(verify_archive(archive), ErrorCode::integrity_error,
                       what + ": verify rejects damaged data");
            const auto target = work / (name + "-damaged-out");
            check_code(extract_archive(archive, target), ErrorCode::integrity_error,
                       what + ": extract rejects damaged data");
            check(!fs::exists(target) && no_leftovers(work), what + ": nothing published");
        }
    }
}
} // namespace

int main(int argc, char *argv[]) {
    if (argc != 2) {
        return 2;
    }
    const auto base = fs::absolute(argv[1]).lexically_normal();
    fs::create_directories(base);
    const auto work = base / ("m3-codec-" + std::to_string(GetCurrentProcessId()) + "-" +
                              std::to_string(GetTickCount64()));
    if (!fs::create_directory(work)) {
        return 2;
    }
    try {
        codec_boundary_tests();
        archive_tests(work);
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        ++failures;
    }
    // Only regular files and directories are created here; refuse to clean anything else.
    const bool safe = work.parent_path() == base && test_fixture::reparse_free(work);
    // Bounded removal instead of remove_all() (libstdc++ can spin on a vanishing entry, DEV-011).
    const auto cleanup =
        safe && failures == 0 ? test_fixture::remove_tree(work) : std::error_code{};
    if (!safe || failures != 0 || cleanup) {
        std::cerr << "fixture retained at " << work << (cleanup ? ": " + cleanup.message() : "")
                  << '\n';
    }
    std::cout << (failures == 0 ? "all codec checks passed" : "codec checks failed") << '\n';
    return failures == 0 ? 0 : 1;
}
