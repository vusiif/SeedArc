// SPDX-License-Identifier: GPL-3.0-or-later
#include "archive_plan.hpp"
#include "seed/archive_study.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <memory>
#include <string_view>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <psapi.h>
#include <shellapi.h>

namespace fs = std::filesystem;
namespace study = seedarc::experimental::seed_study;
using namespace seedarc::experimental;
namespace {
constexpr std::string_view usage =
    "Experimental concrete-plan compression; no seed wire format or search.\n"
    "Usage: seedarc_seed_archive compress MODE INPUT ARCHIVE TARGET_PATH|-\n"
    "       seedarc_seed_archive verify ARCHIVE\n"
    "       seedarc_seed_archive extract ARCHIVE NEW_DIRECTORY\n"
    "       seedarc_seed_archive --version | --help\n"
    "MODE: store, zstd3, zstd6, lzma6, b-base, b-global, b-file, b-cleared.\n"
    "64 KiB chunks. B-r1 codec seed0, LZMA2 preset6. Zstd levels: base3, global4,\n"
    "file6 on TARGET_PATH over global4, cleared4. Target required only for b-file.\n"
    "Paths are UTF-8; target is an exact canonical relative archive path.\n";

void json_string(std::string_view value) {
    std::cout << '"';
    for (const auto raw : value) {
        const auto c = static_cast<unsigned char>(raw);
        if (c == '"' || c == '\\') {
            std::cout << '\\' << raw;
        } else if (c < 32) {
            constexpr char hex[] = "0123456789abcdef";
            std::cout << "\\u00" << hex[c >> 4] << hex[c & 15];
        } else {
            std::cout << raw;
        }
    }
    std::cout << '"';
}
void peak_memory() {
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    std::cout << ",\"peak_working_set_bytes\":";
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
        std::cout << counters.PeakWorkingSetSize;
    } else {
        std::cout << "null";
    }
}
int error(const seedarc::Error &e) {
    std::cerr << e.message << '\n';
    return 1;
}
std::expected<ArchiveIndex, seedarc::Error> read_index(const fs::path &archive) {
    std::ifstream stream(archive, std::ios::binary);
    std::array<std::byte, archive_header_bytes> bytes{};
    if (!stream.read(reinterpret_cast<char *>(bytes.data()), bytes.size())) {
        return std::unexpected(seedarc::Error{seedarc::ErrorCode::io_error, "read header failed"});
    }
    const auto header = decode_header(bytes);
    if (!header) {
        return std::unexpected(header.error());
    }
    stream.seekg(static_cast<std::streamoff>(archive_header_bytes + header->data_bytes));
    std::vector<std::byte> index(static_cast<std::size_t>(header->index_bytes));
    if (!stream.read(reinterpret_cast<char *>(index.data()),
                     static_cast<std::streamsize>(index.size()))) {
        return std::unexpected(seedarc::Error{seedarc::ErrorCode::io_error, "read index failed"});
    }
    return parse_index(*header, index);
}
fs::path native_path(std::string_view text) {
    return fs::path{std::u8string{text.begin(), text.end()}};
}

int run(int argc, char *argv[]) {
    if (argc == 2 && std::string_view{argv[1]} == "--help") {
        std::cout << usage;
        return 0;
    }
    if (argc == 2 && std::string_view{argv[1]} == "--version") {
        std::cout << "SeedArc B2-r3 study; " << codec_library_versions() << '\n';
        return 0;
    }
    if ((argc == 3 && std::string_view{argv[1]} == "verify") ||
        (argc == 4 && std::string_view{argv[1]} == "extract")) {
        const auto result = argc == 3 ? verify_archive(native_path(argv[2]))
                                      : extract_archive(native_path(argv[2]), native_path(argv[3]));
        if (!result) {
            return error(result.error());
        }
        std::cout << "{\"archive_bytes\":" << result->archive_bytes;
        peak_memory();
        std::cout << "}\n";
        return std::cout ? 0 : 1;
    }
    if (argc != 6 || std::string_view{argv[1]} != "compress") {
        std::cerr << usage;
        return 2;
    }
    const std::string_view mode{argv[2]}, target{argv[5]};
    const bool b_mode =
        mode == "b-base" || mode == "b-global" || mode == "b-file" || mode == "b-cleared";
    const bool fixed = mode == "store" || mode == "zstd3" || mode == "zstd6" || mode == "lzma6";
    if ((!b_mode && !fixed) || (mode == "b-file" ? target == "-" : target != "-")) {
        std::cerr << usage;
        return 2;
    }
    CompressOptions options;
    options.chunk_size_bytes = 65536;
    study::B2Compression report;
    const auto input = native_path(argv[3]), archive = native_path(argv[4]);
    if (b_mode) {
        std::vector<study::PathRankOverride> edits;
        if (mode == "b-file") {
            edits.push_back({std::string(target), 5, std::nullopt});
        }
        auto result = study::compress_b2_archive(input, archive, options,
                                                 {0, mode == "b-base" ? 2U : 3U, 6}, edits);
        if (!result) {
            return error(result.error());
        }
        report = std::move(*result);
    } else {
        const auto codec = mode == "store"   ? Codec::store
                           : mode == "lzma6" ? Codec::lzma2
                                             : Codec::zstd;
        const std::uint32_t config = mode == "store" ? 0U : mode == "zstd3" ? 3U : 6U;
        const internal::PlanBuilder builder = [&](std::span<const IndexEntry> entries,
                                                  std::uint64_t count)
            -> std::expected<std::vector<internal::BlockEncoding>, seedarc::Error> {
            if (count > study::max_scope_blocks || entries.size() > study::max_scope_files) {
                return std::unexpected(
                    seedarc::Error{seedarc::ErrorCode::resource_limit, "study limit"});
            }
            for (std::size_t i = 0; i < entries.size(); ++i) {
                if (!entries[i].directory) {
                    report.files.push_back({i, entries[i].path});
                    for (std::uint64_t local = 0; local < entries[i].block_count; ++local) {
                        report.planned.push_back(
                            {{i, local, entries[i].first_block + local}, {codec, config}});
                    }
                }
            }
            return std::vector<internal::BlockEncoding>(static_cast<std::size_t>(count),
                                                        {codec, config});
        };
        const auto result = internal::compress_archive_planned(input, archive, options, builder);
        if (!result) {
            return error(result.error());
        }
        report.summary = *result;
    }
    const auto actual = read_index(archive);
    if (!actual) {
        return error(actual.error());
    }
    if (actual->blocks.size() != report.planned.size()) {
        std::cerr << "plan and archive count mismatch\n";
        return 1;
    }
    std::cout << "{\"schema\":\"seedarc.b2-compression.r3\",\"mode\":";
    json_string(mode);
    std::cout << ",\"chunk_bytes\":65536,\"archive_bytes\":" << report.summary.archive_bytes
              << ",\"input_bytes\":" << report.summary.total_output << ",\"files\":[";
    for (std::size_t i = 0; i < report.files.size(); ++i) {
        if (i != 0) {
            std::cout << ',';
        }
        std::cout << "{\"file_id\":" << report.files[i].file_id << ",\"path\":";
        json_string(report.files[i].path);
        std::cout << '}';
    }
    std::cout << "],\"blocks\":[";
    for (std::size_t i = 0; i < report.planned.size(); ++i) {
        if (i != 0) {
            std::cout << ',';
        }
        const auto &p = report.planned[i];
        const auto &a = actual->blocks[i];
        std::cout << "{\"block_index\":" << i << ",\"block_id\":" << p.identity.global_block_id
                  << ",\"file_id\":" << p.identity.file_id
                  << ",\"file_block_index\":" << p.identity.file_block_index
                  << ",\"planned_codec\":" << static_cast<std::uint32_t>(p.plan.codec)
                  << ",\"planned_config\":" << p.plan.level << ",\"actual_codec\":" << a.codec_id
                  << ",\"actual_config\":" << a.codec_config_id << ",\"raw_bytes\":" << a.raw_size
                  << ",\"stored_bytes\":" << a.stored_size << '}';
    }
    std::cout << ']';
    peak_memory();
    std::cout << "}\n";
    return std::cout ? 0 : 1;
}

} // namespace

int main() {
    int count = 0;
    const auto free_args = [](wchar_t **p) { LocalFree(p); };
    std::unique_ptr<wchar_t *, decltype(free_args)> wide(
        CommandLineToArgvW(GetCommandLineW(), &count), free_args);
    if (!wide) {
        return 1;
    }
    std::vector<std::string> utf8;
    utf8.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.get()[i], -1,
                                             nullptr, 0, nullptr, nullptr);
        if (size == 0) {
            return 1;
        }
        std::string text(static_cast<std::size_t>(size), '\0');
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.get()[i], -1, text.data(), size,
                                nullptr, nullptr) != size) {
            return 1;
        }
        text.pop_back();
        utf8.push_back(std::move(text));
    }
    std::vector<char *> args;
    args.reserve(utf8.size());
    for (auto &text : utf8) {
        args.push_back(text.data());
    }
    return run(count, args.data());
}
