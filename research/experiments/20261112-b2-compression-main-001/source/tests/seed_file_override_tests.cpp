// SPDX-License-Identifier: GPL-3.0-or-later
#include "seed/file_overrides.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <string_view>

namespace study = seedarc::experimental::seed_study;
using seedarc::ErrorCode;
using seedarc::experimental::Codec;

namespace {
int failures = 0;
void check(bool condition, std::string_view description) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << description << '\n';
    }
}

void reject(const study::FieldSeed &defaults, std::span<const study::BlockIdentity> blocks,
            std::span<const study::FileRankOverride> entries, ErrorCode code,
            std::uint32_t revision = 1) {
    const auto result = study::make_file_plans(defaults, blocks, entries, revision);
    check(!result && result.error().code == code, "invalid overlay input rejected");
}
} // namespace

int main() {
    constexpr study::FieldSeed defaults{0, 2, 6};
    std::array<study::BlockIdentity, 12> blocks{};
    for (std::uint64_t id = 0; id < blocks.size(); ++id) {
        blocks[static_cast<std::size_t>(id)] = {id / 4, id % 4, id};
    }
    const auto base = study::make_file_plans(defaults, blocks, {});
    constexpr std::array zstd_edit{study::FileRankOverride{1, 3, std::nullopt}};
    const auto edited = study::make_file_plans(defaults, blocks, zstd_edit);
    if (!base || !edited) {
        std::cerr << "FAIL: base fixture generation\n";
        return 1;
    }
    std::size_t changes = 0;
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        const auto direct = study::make_plan({study::field_candidate, 1}, defaults, i);
        check(direct && (*base)[i].plan == *direct, "base retains B-r1 global identity");
        const bool selected = blocks[i].file_id == 1 && (*base)[i].plan.codec == Codec::zstd;
        check((*edited)[i].identity == blocks[i] &&
                  (*edited)[i].plan.codec == (*base)[i].plan.codec,
              "overlay does not change identity or codec allocation");
        check(selected ? (*edited)[i].plan.level == 4 : (*edited)[i] == (*base)[i],
              "Zstd rank only changes matching codec within selected file");
        changes += (*edited)[i] != (*base)[i] ? 1U : 0U;
    }
    check(changes == 2 && (*edited)[5].plan.level == 4 && (*edited)[6].plan.level == 4,
          "file edit has a nonempty known effect");

    const auto cleared = study::make_file_plans(defaults, blocks, {});
    check(cleared && *cleared == *base, "removing the entry restores defaults");
    constexpr std::array lzma_only{study::FileRankOverride{1, std::nullopt, 3}};
    const auto lzma_edit = study::make_file_plans(defaults, blocks, lzma_only);
    const auto changed_global_zstd = study::make_file_plans({0, 4, 6}, blocks, lzma_only);
    const auto changed_global_lzma = study::make_file_plans({0, 2, 8}, blocks, lzma_only);
    if (!lzma_edit || !changed_global_zstd || !changed_global_lzma) {
        std::cerr << "FAIL: inheritance fixture generation\n";
        return 1;
    }
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        const auto codec = (*base)[i].plan.codec;
        const bool selected = blocks[i].file_id == 1;
        check((*lzma_edit)[i].plan.level ==
                  (selected && codec == Codec::lzma2 ? 3 : (*base)[i].plan.level),
              "LZMA2-only override inherits Zstd and isolates other files");
        check((*changed_global_zstd)[i].plan.level ==
                  (codec == Codec::zstd ? 5 : (*lzma_edit)[i].plan.level),
              "uncovered Zstd field follows changed global defaults in overridden file");
        const auto expected_lzma =
            codec == Codec::lzma2 ? (selected ? 3U : 8U) : (*base)[i].plan.level;
        check((*changed_global_lzma)[i].plan.level == expected_lzma,
              "covered LZMA2 field ignores changed global default while other files inherit");
        check((*changed_global_lzma)[i].plan.codec == codec &&
                  (*changed_global_zstd)[i].plan.codec == codec,
              "global rank edit and file overlay both retain codec allocation");
    }
    constexpr std::array both_fields{study::FileRankOverride{1, 9, 3}};
    const auto both = study::make_file_plans(defaults, blocks, both_fields);
    check(both && (*both)[5].plan.level == 10 && (*both)[4].plan.level == 3 &&
              (*lzma_edit)[5].plan.level == 3 && (*lzma_edit)[4].plan.level == 3,
          "removing one field restores just that field and retains the other override");
    const auto cleared_after_defaults_change = study::make_file_plans({0, 4, 8}, blocks, {});
    check(cleared_after_defaults_change && (*cleared_after_defaults_change)[5].plan.level == 5 &&
              (*cleared_after_defaults_change)[4].plan.level == 8,
          "clear restores current defaults rather than a saved old default");

    std::array reordered_entries{study::FileRankOverride{0, 18, std::nullopt},
                                 study::FileRankOverride{1, std::nullopt, 0}};
    const auto canonical = study::make_file_plans(defaults, blocks, reordered_entries);
    std::ranges::reverse(blocks);
    std::ranges::reverse(reordered_entries);
    const auto reordered = study::make_file_plans(defaults, blocks, reordered_entries);
    check(canonical && reordered && *canonical == *reordered,
          "block and override order do not change canonical output");
    check(reordered &&
              std::ranges::is_sorted(
                  *reordered, {}, [](const auto &block) { return block.identity.global_block_id; }),
          "canonical output sorts original global IDs");

    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    constexpr std::array sparse{study::BlockIdentity{maximum, maximum, maximum},
                                study::BlockIdentity{7, 13, 2}};
    const auto sparse_plan = study::make_file_plans(defaults, sparse, {});
    const auto maximum_direct = study::make_plan({study::field_candidate, 1}, defaults, maximum);
    check(sparse_plan && maximum_direct && sparse_plan->front().identity == sparse[1] &&
              sparse_plan->back().identity == sparse[0] &&
              sparse_plan->back().plan == *maximum_direct,
          "sparse and maximum IDs are preserved without renumbering");

    reject(defaults, blocks, {}, ErrorCode::unsupported_feature, 2);
    reject({0, 19, 0}, {}, {}, ErrorCode::invalid_argument);
    reject({0, 0, 10}, {}, {}, ErrorCode::invalid_argument);
    constexpr std::array invalid_entries{study::FileRankOverride{99, 0, std::nullopt},
                                         study::FileRankOverride{0, std::nullopt, std::nullopt},
                                         study::FileRankOverride{0, 19, std::nullopt},
                                         study::FileRankOverride{0, std::nullopt, 10},
                                         study::FileRankOverride{0, UINT32_MAX, UINT32_MAX}};
    for (const auto &entry : invalid_entries) {
        const std::array entries{entry};
        reject(defaults, blocks, entries, ErrorCode::invalid_argument);
    }
    constexpr std::array duplicate_entries{study::FileRankOverride{0, 1, std::nullopt},
                                           study::FileRankOverride{0, std::nullopt, 1}};
    reject(defaults, blocks, duplicate_entries, ErrorCode::invalid_argument);
    constexpr std::array duplicate_global{study::BlockIdentity{0, 0, 4},
                                          study::BlockIdentity{1, 0, 4}};
    constexpr std::array duplicate_local{study::BlockIdentity{0, 0, 4},
                                         study::BlockIdentity{0, 0, 9}};
    reject(defaults, duplicate_global, {}, ErrorCode::invalid_argument);
    reject(defaults, duplicate_local, {}, ErrorCode::invalid_argument);
    const auto empty = study::make_file_plans(defaults, {}, {});
    check(empty && empty->empty(), "empty valid list is allowed");

    std::vector<study::BlockIdentity> many(study::max_scope_blocks);
    for (std::size_t i = 0; i < many.size(); ++i) {
        many[i] = {0, i, i * 2};
    }
    const auto at_block_limit = study::make_file_plans(defaults, many, {});
    check(at_block_limit && at_block_limit->size() == study::max_scope_blocks,
          "maximum block count is accepted");
    many.push_back({0, maximum, maximum});
    reject(defaults, many, {}, ErrorCode::resource_limit);
    many.resize(study::max_scope_files);
    std::vector<study::FileRankOverride> many_entries;
    for (std::size_t i = 0; i < many.size(); ++i) {
        many[i] = {i, 0, i};
        many_entries.push_back({i, 18, 9});
    }
    const auto at_file_limit = study::make_file_plans(defaults, many, many_entries);
    check(at_file_limit && at_file_limit->size() == study::max_scope_files,
          "maximum files and overrides are accepted with legal rank endpoints");
    many.push_back({study::max_scope_files, 0, study::max_scope_files});
    reject(defaults, many, {}, ErrorCode::resource_limit);
    many_entries.push_back({0, 0, 0});
    reject(defaults, {}, many_entries, ErrorCode::resource_limit);

    std::cout << (failures == 0 ? "all file rank overlay checks passed" : "overlay checks failed")
              << '\n';
    return failures == 0 ? 0 : 1;
}
