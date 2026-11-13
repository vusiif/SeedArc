// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "file_overrides.hpp"

#include <string>

namespace seedarc::experimental::seed_study {
struct PlanFile {
    std::uint64_t file_id;
    std::string path;
};

struct PathRankOverride {
    std::string path;
    std::optional<std::uint32_t> zstd_rank;
    std::optional<std::uint32_t> lzma2_rank;
};

// catalog-r1 binds exact canonical archive paths to the IDs in this invocation.
// Includes empty files; stale/renamed paths fail rather than moving edits to another file.
// Does not assign IDs, migrate renames, or stabilize B-r1's global-block codec mapping.
[[nodiscard]] std::expected<std::vector<PlannedBlock>, Error>
make_path_file_plans(const FieldSeed &defaults, std::span<const PlanFile> files,
                     std::span<const BlockIdentity> blocks,
                     std::span<const PathRankOverride> overrides,
                     std::uint32_t catalog_revision = 1);
} // namespace seedarc::experimental::seed_study
