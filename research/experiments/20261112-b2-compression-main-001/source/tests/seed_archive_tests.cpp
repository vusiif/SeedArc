// SPDX-License-Identifier: GPL-3.0-or-later
#include "archive_plan.hpp"
#include "seed/archive_study.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace fs = std::filesystem;
namespace study = seedarc::experimental::seed_study;
using namespace seedarc::experimental;
using seedarc::ErrorCode;
namespace {
int failures = 0;
void check(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}
std::vector<std::byte> read(const fs::path &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("fixture read failed");
    }
    std::vector<char> chars{std::istreambuf_iterator<char>{file}, {}};
    std::vector<std::byte> bytes(chars.size());
    std::ranges::transform(chars, bytes.begin(), [](char c) { return static_cast<std::byte>(c); });
    return bytes;
}
void write(const fs::path &path, std::span<const std::byte> bytes) {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    if (!file) {
        throw std::runtime_error("fixture write failed");
    }
}
ArchiveIndex index_of(const std::vector<std::byte> &bytes) {
    const auto header = decode_header(std::span{bytes}.first(archive_header_bytes));
    if (!header) {
        throw std::runtime_error("fixture header invalid");
    }
    const auto index =
        parse_index(*header, std::span{bytes}.subspan(archive_header_bytes + header->data_bytes));
    if (!index) {
        throw std::runtime_error("fixture index invalid");
    }
    check(header->strategy_version == 0 && header->seed_length == 0,
          "archive has no seed/search decoding dependency");
    return *index;
}
template <class T>
void rejected(const std::expected<T, seedarc::Error> &r, ErrorCode code, const fs::path &archive) {
    check(!r && r.error().code == code, "expected failure propagated");
    check(!fs::exists(archive), "failed operation does not publish output");
}
} // namespace

int main(int argc, char *argv[]) {
    if (argc != 2) {
        return 2;
    }
    const auto parent = fs::absolute(argv[1]);
    fs::create_directories(parent);
    const auto work = parent / ("seed-archive-" + std::to_string(GetCurrentProcessId()));
    if (!fs::create_directory(work)) {
        std::cerr << "test directory already exists\n";
        return 1;
    }
    try {
        const auto input = work / "input";
        fs::create_directory(input);
        std::vector<std::byte> text(12 * 4096, std::byte{'a'});
        write(input / "target.txt", text);
        write(input / "empty", {});
        // A single byte can never be compressed smaller: fallback is nonvacuous for any codec.
        constexpr std::array tiny{std::byte{77}};
        write(input / "tiny.bin", tiny);
        CompressOptions options;
        options.chunk_size_bytes = 4096;
        const std::array edits{study::PathRankOverride{"target.txt", 5, std::nullopt}};
        const auto base =
            study::compress_b2_archive(input, work / "base.sarc", options, {0, 2, 6}, {});
        const auto global =
            study::compress_b2_archive(input, work / "global.sarc", options, {0, 3, 6}, {});
        const auto edited =
            study::compress_b2_archive(input, work / "edited.sarc", options, {0, 3, 6}, edits);
        const auto cleared =
            study::compress_b2_archive(input, work / "cleared.sarc", options, {0, 3, 6}, {});
        check(base && global && edited && cleared, "four real archives created");
        if (!base || !global || !edited || !cleared) {
            throw std::runtime_error("fixture compression failed");
        }
        check(read(work / "global.sarc") == read(work / "cleared.sarc"),
              "clear reproduces current-global archive byte for byte");
        std::size_t zstd_blocks = 0;
        for (const auto &name : {"base", "global", "edited", "cleared"}) {
            const auto archive = work / (std::string(name) + ".sarc");
            check(verify_archive(archive).has_value(), "verify without research planner");
            const auto out = work / (std::string(name) + "-out");
            check(extract_archive(archive, out).has_value(), "extract without research planner");
            check(read(out / "target.txt") == text &&
                      read(out / "tiny.bin") == std::vector<std::byte>(tiny.begin(), tiny.end()) &&
                      read(out / "empty").empty(),
                  "round trip is byte exact including empty file");
        }
        const auto actual = index_of(read(work / "edited.sarc"));
        check(actual.blocks.size() == edited->planned.size(), "actual count matches plan");
        for (std::size_t i = 0; i < edited->planned.size(); ++i) {
            const auto &p = edited->planned[i];
            const auto &b = actual.blocks[i];
            check(p.identity.global_block_id == i, "actual writer ordinal is logical ID in r3");
            check(p.plan.codec == global->planned[i].plan.codec,
                  "file overlay keeps codec schedule");
            if (b.codec_id != 0) {
                check(b.codec_id == static_cast<std::uint32_t>(p.plan.codec) &&
                          b.codec_config_id == p.plan.level,
                      "executed codec/config matches plan");
                zstd_blocks += b.codec_id == 1 ? 1U : 0U;
            } else {
                check(b.codec_config_id == 0 && b.stored_size == b.raw_size,
                      "Store fallback is explicit in actual descriptor");
            }
        }
        check(zstd_blocks != 0 && actual.blocks.back().codec_id == 0,
              "actual Zstd and tiny-block Store fallback are both exercised");
        const std::array empty_edit{study::PathRankOverride{"empty", 5, std::nullopt}};
        const auto empty_result = study::compress_b2_archive(input, work / "empty-edit.sarc",
                                                             options, {0, 3, 6}, empty_edit);
        check(empty_result && read(work / "empty-edit.sarc") == read(work / "global.sarc"),
              "empty-file override is a real archive no-op");
        const std::array tiny_edit{study::PathRankOverride{"tiny.bin", 5, std::nullopt}};
        const auto tiny_base = study::compress_b2_archive(
            input / "tiny.bin", work / "tiny-base.sarc", options, {0, 3, 6}, {});
        const auto tiny_edited = study::compress_b2_archive(
            input / "tiny.bin", work / "tiny-edit.sarc", options, {0, 3, 6}, tiny_edit);
        check(tiny_base && tiny_edited && tiny_base->planned[0].plan.codec == Codec::lzma2 &&
                  read(work / "tiny-base.sarc") == read(work / "tiny-edit.sarc"),
              "real no-target-codec file override leaves archive unchanged");
        for (const auto selected_codec : {Codec::store, Codec::zstd, Codec::lzma2}) {
            const auto public_path = work / "fixed-public.sarc";
            const auto planned_path = work / "fixed-planned.sarc";
            auto fixed = options;
            fixed.codec = selected_codec;
            fixed.level = selected_codec == Codec::store ? std::nullopt : std::optional{6U};
            const internal::PlanBuilder builder = [selected_codec](std::span<const IndexEntry>,
                                                                   std::uint64_t n)
                -> std::expected<std::vector<internal::BlockEncoding>, seedarc::Error> {
                return std::vector<internal::BlockEncoding>(
                    static_cast<std::size_t>(n),
                    {selected_codec, selected_codec == Codec::store ? 0U : 6U});
            };
            const auto fixed_result = compress_archive(input, public_path, fixed);
            const auto planned_result =
                internal::compress_archive_planned(input, planned_path, options, builder);
            check(fixed_result && planned_result && read(public_path) == read(planned_path),
                  "fixed planned encoding matches existing public path byte for byte");
            fs::remove(public_path);
            fs::remove(planned_path);
        }
        const auto original_bytes = read(work / "base.sarc");
        const auto conflict =
            study::compress_b2_archive(input, work / "base.sarc", options, {0, 3, 6}, edits);
        check(!conflict && conflict.error().code == ErrorCode::output_conflict &&
                  read(work / "base.sarc") == original_bytes,
              "existing archive never overwritten");
        const auto before_failures =
            std::distance(fs::directory_iterator(work), fs::directory_iterator{});
        const auto bad = work / "bad.sarc";
        const std::array stale{study::PathRankOverride{"missing", 5, std::nullopt}};
        rejected(study::compress_b2_archive(input, bad, options, {0, 3, 6}, stale),
                 ErrorCode::invalid_argument, bad);
        rejected(study::compress_b2_archive(input, bad, options, {0, 3, 6}, {}, {2, 99}),
                 ErrorCode::unsupported_feature, bad);
        rejected(study::compress_b2_archive(input, bad, options, {0, 19, 6}, {}),
                 ErrorCode::invalid_argument, bad);
        std::stop_source stopped;
        stopped.request_stop();
        rejected(study::compress_b2_archive(input, bad, options, {0, 3, 6}, {}, {2, 1}, 1,
                                            stopped.get_token()),
                 ErrorCode::cancelled, bad);
        for (int mode = 0; mode != 5; ++mode) {
            const internal::PlanBuilder builder = [mode](std::span<const IndexEntry>,
                                                         std::uint64_t count)
                -> std::expected<std::vector<internal::BlockEncoding>, seedarc::Error> {
                if (mode == 0) {
                    return std::unexpected(
                        seedarc::Error{ErrorCode::invalid_argument, "planner failed"});
                }
                std::vector<internal::BlockEncoding> plans(static_cast<std::size_t>(count),
                                                           {Codec::store, 0});
                if (mode == 1) {
                    plans.pop_back();
                }
                if (mode == 2) {
                    plans[0] = {static_cast<Codec>(99), 0};
                }
                if (mode == 3) {
                    plans[0] = {Codec::zstd, 0};
                }
                if (mode == 4) {
                    plans[0] = {Codec::store, 1};
                }
                return plans;
            };
            rejected(internal::compress_archive_planned(input, bad, options, builder),
                     ErrorCode::invalid_argument, bad);
        }
        const internal::PlanBuilder empty_builder;
        rejected(internal::compress_archive_planned(input, bad, options, empty_builder),
                 ErrorCode::invalid_argument, bad);
        // Even cancellation requested by the planner is observed before publication.
        std::stop_source during;
        const internal::PlanBuilder cancel_builder = [&](std::span<const IndexEntry>,
                                                         std::uint64_t count)
            -> std::expected<std::vector<internal::BlockEncoding>, seedarc::Error> {
            during.request_stop();
            return std::vector<internal::BlockEncoding>(static_cast<std::size_t>(count),
                                                        {Codec::store, 0});
        };
        rejected(internal::compress_archive_planned(input, bad, options, cancel_builder,
                                                    during.get_token()),
                 ErrorCode::cancelled, bad);
        const internal::PlanBuilder mutate_builder = [&](std::span<const IndexEntry>,
                                                         std::uint64_t count)
            -> std::expected<std::vector<internal::BlockEncoding>, seedarc::Error> {
            std::ofstream changed(input / "target.txt", std::ios::binary | std::ios::app);
            changed.put('x');
            changed.close();
            return std::vector<internal::BlockEncoding>(static_cast<std::size_t>(count),
                                                        {Codec::store, 0});
        };
        const auto mutated =
            internal::compress_archive_planned(input, bad, options, mutate_builder);
        check(!mutated && !fs::exists(bad),
              "input mutation after planning is rejected without publication");
        const auto names = std::distance(fs::directory_iterator(work), fs::directory_iterator{});
        check(names == before_failures, "failed planned writes leave no temporary artifacts");
        auto broken = read(work / "edited.sarc");
        broken.pop_back();
        check(!verify_archive_bytes(broken), "truncated new archive rejected");
        broken = read(work / "edited.sarc");
        broken[archive_header_bytes] ^= std::byte{1};
        check(!verify_archive_bytes(broken), "corrupted block rejected");
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        ++failures;
    }
    bool safe = work.parent_path() == parent;
    for (const auto &entry : fs::recursive_directory_iterator(work)) {
        const auto attrs = GetFileAttributesW(entry.path().c_str());
        safe =
            safe && attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
    }
    if (safe && failures == 0) {
        fs::remove_all(work);
    } else {
        std::cerr << "fixture retained at " << work << '\n';
    }
    std::cout << (failures == 0 ? "all seed archive checks passed" : "seed archive checks failed")
              << '\n';
    return failures == 0 ? 0 : 1;
}
