// SPDX-License-Identifier: GPL-3.0-or-later
#include "seed/file_overrides.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <utility>

namespace seedarc::experimental::seed_study {
namespace {
Error invalid(const char *message) { return {ErrorCode::invalid_argument, message}; }
} // namespace

std::expected<std::vector<PlannedBlock>, Error>
make_file_plans(const FieldSeed &defaults, std::span<const BlockIdentity> blocks,
                std::span<const FileRankOverride> overrides, std::uint32_t overlay_revision) {
    if (overlay_revision != 1) {
        return std::unexpected(
            Error{ErrorCode::unsupported_feature, "unknown file rank overlay revision"});
    }
    if (blocks.size() > max_scope_blocks || overrides.size() > max_file_overrides) {
        return std::unexpected(Error{ErrorCode::resource_limit, "file rank overlay input limit"});
    }
    // Validate even an empty block list; an invalid default must never become a valid no-op.
    constexpr Candidate base_candidate{field_candidate, 1};
    if (const auto checked = make_plan(base_candidate, defaults, 0); !checked) {
        return std::unexpected(checked.error());
    }

    std::vector<BlockIdentity> ordered(blocks.begin(), blocks.end());
    std::ranges::sort(ordered, {}, &BlockIdentity::global_block_id);
    std::set<std::uint64_t> files;
    std::set<std::pair<std::uint64_t, std::uint64_t>> local_ids;
    for (std::size_t i = 0; i < ordered.size(); ++i) {
        const auto &block = ordered[i];
        if (i != 0 && ordered[i - 1].global_block_id == block.global_block_id) {
            return std::unexpected(invalid("duplicate global block ID"));
        }
        if (!local_ids.emplace(block.file_id, block.file_block_index).second) {
            return std::unexpected(invalid("duplicate file-local block identity"));
        }
        files.insert(block.file_id);
        if (files.size() > max_scope_files) {
            return std::unexpected(
                Error{ErrorCode::resource_limit, "file rank overlay file count limit"});
        }
    }

    std::map<std::uint64_t, FileRankOverride> by_file;
    for (const auto &entry : overrides) {
        if (!files.contains(entry.file_id)) {
            return std::unexpected(invalid("file rank override references an unknown file ID"));
        }
        if ((!entry.zstd_rank && !entry.lzma2_rank) || (entry.zstd_rank && *entry.zstd_rank > 18) ||
            (entry.lzma2_rank && *entry.lzma2_rank > 9)) {
            return std::unexpected(invalid("empty or invalid file rank override"));
        }
        if (!by_file.emplace(entry.file_id, entry).second) {
            return std::unexpected(invalid("duplicate file rank override"));
        }
    }

    std::vector<PlannedBlock> result;
    result.reserve(ordered.size());
    for (const auto &block : ordered) {
        auto fields = defaults;
        if (const auto found = by_file.find(block.file_id); found != by_file.end()) {
            const auto &entry = found->second;
            fields.zstd_rank = entry.zstd_rank.value_or(defaults.zstd_rank);
            fields.lzma2_rank = entry.lzma2_rank.value_or(defaults.lzma2_rank);
        }
        // Rank fields do not participate in B-r1's codec mapping. Keeping the original global
        // ID preserves the original allocation exactly; file/local IDs only select the overlay.
        const auto plan = make_plan(base_candidate, fields, block.global_block_id);
        if (!plan) {
            return std::unexpected(plan.error());
        }
        result.push_back({block, *plan});
    }
    return result;
}
} // namespace seedarc::experimental::seed_study
