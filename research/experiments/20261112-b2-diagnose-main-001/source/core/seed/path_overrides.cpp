// SPDX-License-Identifier: GPL-3.0-or-later
#include "seed/path_overrides.hpp"

#include "seedarc/archive_path.hpp"

#include <map>
#include <set>

namespace seedarc::experimental::seed_study {
namespace {
Error invalid(const char *message) { return {ErrorCode::invalid_argument, message}; }
} // namespace

std::expected<std::vector<PlannedBlock>, Error>
make_path_file_plans(const FieldSeed &defaults, std::span<const PlanFile> files,
                     std::span<const BlockIdentity> blocks,
                     std::span<const PathRankOverride> overrides, std::uint32_t catalog_revision) {
    if (catalog_revision != 1) {
        return std::unexpected(
            Error{ErrorCode::unsupported_feature, "unknown path catalog revision"});
    }
    if (files.size() > max_scope_files || blocks.size() > max_scope_blocks ||
        overrides.size() > max_file_overrides) {
        return std::unexpected(Error{ErrorCode::resource_limit, "path catalog input limit"});
    }
    // Check the aggregate budget before copying any strings into validation structures.
    constexpr std::size_t metadata_budget = 16 * 1024 * 1024;
    std::size_t bytes = 0;
    for (const auto &file : files) {
        if (file.path.size() > 4096 || file.path.size() + 32 > metadata_budget - bytes) {
            return std::unexpected(Error{ErrorCode::resource_limit, "path catalog byte limit"});
        }
        bytes += file.path.size() + 32;
    }
    std::vector<PathEntry> paths;
    paths.reserve(files.size());
    std::map<std::string, std::uint64_t, std::less<>> by_path;
    std::set<std::uint64_t> file_ids;
    for (const auto &file : files) {
        if (!file_ids.insert(file.file_id).second) {
            return std::unexpected(invalid("duplicate catalog file ID"));
        }
        paths.push_back({file.path, false});
        by_path.emplace(file.path, file.file_id);
    }
    // Uses the existing Windows NFC UTF-8 profile, including case-collision rejection.
    if (const auto checked = validate_manifest_paths(paths); !checked) {
        return std::unexpected(checked.error());
    }
    std::set<std::uint64_t> nonempty;
    for (const auto &block : blocks) {
        if (!file_ids.contains(block.file_id)) {
            return std::unexpected(invalid("block references an unknown catalog file ID"));
        }
        nonempty.insert(block.file_id);
    }
    std::set<std::uint64_t> edited;
    std::vector<FileRankOverride> resolved;
    resolved.reserve(overrides.size());
    for (const auto &entry : overrides) {
        // No string copies here; lookup is bounded even for an invalid oversized key.
        if (entry.path.size() > 4096) {
            return std::unexpected(Error{ErrorCode::resource_limit, "override path byte limit"});
        }
        const auto found = by_path.find(entry.path);
        if (found == by_path.end()) {
            return std::unexpected(invalid("override path has no exact catalog match"));
        }
        if (!edited.insert(found->second).second) {
            return std::unexpected(invalid("duplicate path override"));
        }
        if (!entry.zstd_rank && !entry.lzma2_rank) {
            return std::unexpected(invalid("empty path override"));
        }
        auto fields = defaults;
        fields.zstd_rank = entry.zstd_rank.value_or(defaults.zstd_rank);
        fields.lzma2_rank = entry.lzma2_rank.value_or(defaults.lzma2_rank);
        if (const auto checked = make_plan({field_candidate, 1}, fields, 0); !checked) {
            return std::unexpected(checked.error());
        }
        // An empty file is known and editable but has no blocks to modify.
        if (nonempty.contains(found->second)) {
            resolved.push_back({found->second, entry.zstd_rank, entry.lzma2_rank});
        }
    }
    return make_file_plans(defaults, blocks, resolved);
}
} // namespace seedarc::experimental::seed_study
