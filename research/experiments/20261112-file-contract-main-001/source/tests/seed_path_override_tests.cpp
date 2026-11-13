// SPDX-License-Identifier: GPL-3.0-or-later
#include "seed/path_overrides.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <string_view>

namespace study = seedarc::experimental::seed_study;
using seedarc::ErrorCode;
using seedarc::experimental::Codec;
namespace {
int failures = 0;
void check(bool condition, std::string_view message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}
void reject(std::span<const study::PlanFile> files, std::span<const study::BlockIdentity> blocks,
            std::span<const study::PathRankOverride> edits, ErrorCode code,
            std::uint32_t revision = 1) {
    const auto result = study::make_path_file_plans({0, 2, 6}, files, blocks, edits, revision);
    check(!result && result.error().code == code, "invalid catalog rejected with expected error");
}
} // namespace

int main() {
    std::vector<study::PlanFile> files{
        {0, "a.txt"}, {1, "dir/b.txt"}, {2, "c.txt"}, {3, "empty.txt"}};
    std::vector<study::BlockIdentity> blocks;
    for (std::uint64_t i = 0; i < 12; ++i) {
        blocks.push_back({i / 4, i % 4, i});
    }
    const std::array edit{study::PathRankOverride{"dir/b.txt", 5, std::nullopt}};
    const auto base = study::make_path_file_plans({0, 2, 6}, files, blocks, {});
    const auto global = study::make_path_file_plans({0, 3, 6}, files, blocks, {});
    const auto overlay = study::make_path_file_plans({0, 3, 6}, files, blocks, edit);
    const auto cleared = study::make_path_file_plans({0, 3, 6}, files, blocks, {});
    if (!base || !global || !overlay || !cleared) {
        std::cerr << "FAIL: fixture setup\n";
        return 1;
    }
    std::size_t target_count = 0;
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        const auto codec = (*base)[i].plan.codec;
        const bool selected = blocks[i].file_id == 1;
        check((*global)[i].plan.codec == codec && (*overlay)[i].plan.codec == codec,
              "edits retain codec allocation");
        if (codec == Codec::zstd) {
            check((*base)[i].plan.level == 3 && (*global)[i].plan.level == 4 &&
                      (*overlay)[i].plan.level == (selected ? 6U : 4U) &&
                      (*cleared)[i].plan.level == 4,
                  "Zstd sequence 3 -> 4 -> 6 -> 4 follows current defaults");
            target_count += selected ? 1U : 0U;
        } else {
            check((*overlay)[i].plan == (*base)[i].plan, "other codecs unchanged");
        }
    }
    check(target_count == 2, "target sequence is nonvacuous");
    check(*cleared == *global, "clear removes layer rather than restoring old snapshot");

    // Insertion, deletion of an unrelated file, and ordering keep original global IDs.
    files.insert(files.begin(), {99, "0-inserted.txt"});
    blocks.push_back({99, 0, 99});
    std::ranges::reverse(files);
    std::ranges::reverse(blocks);
    auto shifted = study::make_path_file_plans({0, 3, 6}, files, blocks, edit);
    check(shifted && shifted->size() == overlay->size() + 1 &&
              std::equal(overlay->begin(), overlay->end(), shifted->begin()),
          "insert/reorder retains plans with original IDs");
    std::erase_if(files, [](const auto &f) { return f.path == "a.txt"; });
    std::erase_if(blocks, [](const auto &b) { return b.file_id == 0; });
    // Change file IDs as a real list rebuild may do; path is the binding key.
    for (auto &f : files) {
        if (f.path == "dir/b.txt") {
            f.file_id = 77;
        }
    }
    for (auto &b : blocks) {
        if (b.file_id == 1) {
            b.file_id = 77;
        }
        b.global_block_id += 100; // Mapping may change; only binding must remain stable.
    }
    shifted = study::make_path_file_plans({0, 3, 6}, files, blocks, edit);
    check(shifted.has_value(), "ID rebuild resolves the same path");
    std::size_t remapped_targets = 0;
    if (shifted) {
        for (const auto &b : *shifted) {
            const auto direct = study::make_plan(
                {study::field_candidate, 1}, study::FieldSeed{0, 3, 6}, b.identity.global_block_id);
            const bool selected = b.identity.file_id == 77 && b.plan.codec == Codec::zstd;
            check(direct && b.plan.codec == direct->codec &&
                      b.plan.level == (selected ? 6U : direct->level),
                  "rebuilt IDs do not transfer edit to unrelated path");
            remapped_targets += selected ? 1U : 0U;
        }
    }
    check(remapped_targets != 0, "rebuilt-ID target fixture is nonvacuous");
    for (auto &f : files) {
        if (f.file_id == 77) {
            f.path = "dir/renamed.txt";
        }
    }
    reject(files, blocks, edit, ErrorCode::invalid_argument);
    std::erase_if(files, [](const auto &f) { return f.file_id == 77; });
    std::erase_if(blocks, [](const auto &b) { return b.file_id == 77; });
    reject(files, blocks, edit, ErrorCode::invalid_argument);

    const std::array special_files{study::PlanFile{0, "empty.txt"},
                                   study::PlanFile{1, "single.txt"},
                                   study::PlanFile{2, "large.txt"}};
    std::vector<study::BlockIdentity> large{{1, 0, 0}}; // B-r1 global0 is LZMA2.
    for (std::uint64_t i = 0; i < 10000; ++i) {
        large.push_back({2, i, i + 1});
    }
    const std::array empty_edit{study::PathRankOverride{"empty.txt", 5, std::nullopt}};
    const std::array no_target_edit{study::PathRankOverride{"single.txt", 5, std::nullopt}};
    const std::array large_edit{study::PathRankOverride{"large.txt", 5, std::nullopt}};
    const auto large_base = study::make_path_file_plans({0, 3, 6}, special_files, large, {});
    const auto empty_result =
        study::make_path_file_plans({0, 3, 6}, special_files, large, empty_edit);
    const auto no_target =
        study::make_path_file_plans({0, 3, 6}, special_files, large, no_target_edit);
    const auto large_result =
        study::make_path_file_plans({0, 3, 6}, special_files, large, large_edit);
    check(large_base && empty_result && *large_base == *empty_result,
          "explicit empty file is a valid no-change target");
    check(large_base && no_target && *large_base == *no_target,
          "file without target codec has no changes");
    std::size_t large_changes = 0;
    if (large_base && large_result) {
        for (std::size_t i = 0; i < large.size(); ++i) {
            const bool target = large[i].file_id == 2 && (*large_base)[i].plan.codec == Codec::zstd;
            check(target ? (*large_result)[i].plan.level == 6
                         : (*large_result)[i] == (*large_base)[i],
                  "10000-block file changes only target codec");
            large_changes += target ? 1U : 0U;
        }
    }
    check(large_changes > 100, "large fixture has many genuine changes");

    const std::array one{study::PlanFile{0, "ok.txt"}};
    constexpr std::array unknown_block{study::BlockIdentity{99, 0, 0}};
    reject(one, unknown_block, {}, ErrorCode::invalid_argument);
    reject(one, {}, {}, ErrorCode::unsupported_feature, 2);
    const auto all_empty = study::make_path_file_plans({0, 2, 6}, one, {}, {});
    check(all_empty && all_empty->empty(), "all-empty catalog accepted");
    const auto bad_default = study::make_path_file_plans({0, 19, 6}, {}, {}, {});
    check(!bad_default, "invalid defaults rejected without blocks");
    for (const auto &bad : std::array{study::PathRankOverride{"ok.txt", 19, std::nullopt},
                                      study::PathRankOverride{"ok.txt", std::nullopt, 10},
                                      study::PathRankOverride{"ok.txt", std::nullopt, std::nullopt},
                                      study::PathRankOverride{"OK.txt", 5, std::nullopt},
                                      study::PathRankOverride{"missing.txt", 5, std::nullopt}}) {
        const std::array entries{bad};
        reject(one, {}, entries, ErrorCode::invalid_argument);
    }
    const std::array duplicates{study::PathRankOverride{"ok.txt", 5, std::nullopt},
                                study::PathRankOverride{"ok.txt", std::nullopt, 3}};
    reject(one, {}, duplicates, ErrorCode::invalid_argument);
    const std::array duplicate_id{study::PlanFile{0, "a"}, study::PlanFile{0, "b"}};
    reject(duplicate_id, {}, {}, ErrorCode::invalid_argument);
    const std::array case_collision{study::PlanFile{0, "a"}, study::PlanFile{1, "A"}};
    reject(case_collision, {}, {}, ErrorCode::unsafe_path);
    const std::array duplicate_path{study::PlanFile{0, "a"}, study::PlanFile{1, "a"}};
    reject(duplicate_path, {}, {}, ErrorCode::unsafe_path);
    for (const auto path : {"../bad", "a\\b", "/absolute", "CON", "e\xcc\x81.txt"}) {
        const std::array bad{study::PlanFile{0, path}};
        reject(bad, {}, {}, ErrorCode::unsafe_path);
    }
    const std::array unicode{study::PlanFile{0, "\xc3\xa9.txt"}};
    const std::array unicode_edit{study::PathRankOverride{"\xc3\xa9.txt", 5, std::nullopt}};
    check(study::make_path_file_plans({0, 2, 6}, unicode, {}, unicode_edit).has_value(),
          "exact NFC Unicode path works for empty file");
    std::vector<study::PlanFile> many;
    for (std::size_t i = 0; i <= study::max_scope_files; ++i) {
        many.push_back({i, "f" + std::to_string(i)});
    }
    reject(many, {}, {}, ErrorCode::resource_limit);
    many.pop_back();
    check(study::make_path_file_plans({0, 2, 6}, many, {}, {}).has_value(),
          "maximum empty-file catalog accepted");
    const std::array oversized{study::PlanFile{0, std::string(4097, 'a')}};
    reject(oversized, {}, {}, ErrorCode::resource_limit);
    std::vector<study::PathRankOverride> too_many(study::max_file_overrides + 1);
    reject(one, {}, too_many, ErrorCode::resource_limit);
    std::vector<study::BlockIdentity> too_many_blocks(study::max_scope_blocks + 1);
    reject(one, too_many_blocks, {}, ErrorCode::resource_limit);
    std::cout << (failures == 0 ? "all path catalog checks passed" : "catalog checks failed")
              << '\n';
    return failures == 0 ? 0 : 1;
}
