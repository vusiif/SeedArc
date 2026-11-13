// SPDX-License-Identifier: GPL-3.0-or-later
#include "seed/path_overrides.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <string_view>

namespace study = seedarc::experimental::seed_study;
using seedarc::experimental::Codec;
namespace {
void write_plan(const study::BlockPlan &plan) {
    const auto *codec = plan.codec == Codec::store  ? "store"
                        : plan.codec == Codec::zstd ? "zstd"
                                                    : "lzma2";
    std::cout << "{\"codec\":\"" << codec << "\",\"level\":" << plan.level << '}';
}

bool observe(std::string_view name, const std::vector<study::PlanFile> &files,
             const std::vector<study::BlockIdentity> &blocks, std::string_view target) {
    const std::array edits{study::PathRankOverride{std::string(target), 5, std::nullopt}};
    const std::array stages{study::make_path_file_plans({0, 2, 6}, files, blocks, {}),
                            study::make_path_file_plans({0, 3, 6}, files, blocks, {}),
                            study::make_path_file_plans({0, 3, 6}, files, blocks, edits),
                            study::make_path_file_plans({0, 3, 6}, files, blocks, {})};
    for (const auto &stage : stages) {
        if (!stage) {
            std::cerr << stage.error().message << '\n';
            return false;
        }
    }
    // All strings are fixed ASCII fixtures, never user-provided JSON fragments.
    std::cout << "{\"name\":\"" << name << "\",\"target_path\":\"" << target << "\",\"files\":[";
    for (std::size_t i = 0; i < files.size(); ++i) {
        if (i != 0) {
            std::cout << ',';
        }
        std::cout << "{\"file_id\":" << files[i].file_id << ",\"path\":\"" << files[i].path
                  << "\"}";
    }
    std::cout << "],\"blocks\":[";
    std::size_t changes = 0;
    for (std::size_t i = 0; i < stages[0]->size(); ++i) {
        if (i != 0) {
            std::cout << ',';
        }
        const auto &id = (*stages[0])[i].identity;
        std::cout << "{\"file_id\":" << id.file_id
                  << ",\"file_block_index\":" << id.file_block_index
                  << ",\"global_block_id\":" << id.global_block_id;
        for (std::size_t j = 0; const auto *key : {"base", "global", "file", "cleared"}) {
            std::cout << ",\"" << key << "\":";
            write_plan((*stages[j++])[i].plan);
        }
        std::cout << '}';
        changes += (*stages[1])[i].plan != (*stages[2])[i].plan ? 1U : 0U;
    }
    std::cout << "],\"file_changes_from_global\":" << changes << ",\"effect\":\""
              << (changes == 0 ? "no_target_codec_blocks" : "target_codec_levels_changed") << "\"}";
    return true;
}
} // namespace

int main(int argc, char *argv[]) {
    constexpr std::string_view usage =
        "Usage: seedarc_seed_scope_contract --observe | --help\n"
        "Fixed catalog-r1/B-r1 plan-only identity and edit contract fixtures.\n"
        "Zstd levels: base3 -> global4 -> target6 -> clear4; codec seed0, LZMA2 preset6.\n"
        "No real file I/O, compression, search, or seed serialization.\n";
    if (argc == 2 && std::string_view{argv[1]} == "--help") {
        std::cout << usage;
        return 0;
    }
    if (argc != 2 || std::string_view{argv[1]} != "--observe") {
        std::cerr << usage;
        return 2;
    }
    std::vector<study::PlanFile> files{
        {0, "a.txt"}, {1, "dir/b.txt"}, {2, "c.txt"}, {3, "empty.txt"}};
    std::vector<study::BlockIdentity> blocks;
    for (std::uint64_t i = 0; i < 12; ++i) {
        blocks.push_back({i / 4, i % 4, i});
    }
    std::cout << "{\"schema\":\"seedarc.file-contract.r2\",\"plan_only\":true,"
                 "\"catalog_revision\":1,\"overlay_revision\":1,"
                 "\"base_candidate\":{\"id\":2,\"revision\":1},"
                 "\"config\":{\"codec_seed\":0,\"base_zstd_rank\":2,\"global_zstd_rank\":3,"
                 "\"file_zstd_rank\":5,\"lzma2_rank\":6},\"scenarios\":[";
    if (!observe("combined", files, blocks, "dir/b.txt")) {
        return 1;
    }
    files.insert(files.begin(), {99, "0-inserted.txt"});
    blocks.push_back({99, 0, 99});
    std::ranges::reverse(files);
    std::ranges::reverse(blocks);
    std::cout << ',';
    if (!observe("insert-reorder", files, blocks, "dir/b.txt")) {
        return 1;
    }
    std::erase_if(files, [](const auto &f) { return f.file_id == 0; });
    std::erase_if(blocks, [](const auto &b) { return b.file_id == 0; });
    for (auto &f : files) {
        if (f.file_id == 1) {
            f.file_id = 77;
        }
    }
    for (auto &b : blocks) {
        if (b.file_id == 1) {
            b.file_id = 77;
        }
        b.global_block_id += 100;
    }
    std::cout << ',';
    if (!observe("delete-reindex", files, blocks, "dir/b.txt")) {
        return 1;
    }
    const std::array stale{study::PathRankOverride{"dir/b.txt", 5, std::nullopt}};
    for (auto &f : files) {
        if (f.file_id == 77) {
            f.path = "dir/renamed.txt";
        }
    }
    const auto renamed = study::make_path_file_plans({0, 3, 6}, files, blocks, stale);
    std::erase_if(files, [](const auto &f) { return f.file_id == 77; });
    std::erase_if(blocks, [](const auto &b) { return b.file_id == 77; });
    const auto removed = study::make_path_file_plans({0, 3, 6}, files, blocks, stale);
    if (renamed || removed || renamed.error().code != seedarc::ErrorCode::invalid_argument ||
        removed.error().code != seedarc::ErrorCode::invalid_argument) {
        std::cerr << "stale override unexpectedly accepted\n";
        return 1;
    }
    files = {{0, "empty.txt"}, {1, "single.txt"}, {2, "large.txt"}};
    blocks = {{1, 0, 0}};
    for (std::uint64_t i = 0; i < 128; ++i) {
        blocks.push_back({2, i, i + 1});
    }
    for (const auto target : {"empty.txt", "single.txt", "large.txt"}) {
        std::cout << ',';
        if (!observe(target, files, blocks, target)) {
            return 1;
        }
    }
    std::cout << "],\"stale_path_checks\":{\"renamed\":\"invalid_argument\","
                 "\"removed\":\"invalid_argument\"}}\n";
    return std::cout ? 0 : 1;
}
