// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "path_overrides.hpp"

namespace seedarc::experimental::seed_study {
struct B2Compression {
    ArchiveSummary summary;
    std::vector<PlanFile> files; // file_id is the actual manifest entry index, not stable identity.
    std::vector<PlannedBlock> planned;
};

// r3 integration of existing B-r1/catalog-r1 semantics, not a new seed wire representation.
// global_block_id equals the writer's block ordinal for this invocation (0..N-1).
// Planning runs on the same validated manifest the writer uses, before content is read.
[[nodiscard]] std::expected<B2Compression, Error>
compress_b2_archive(const std::filesystem::path &input, const std::filesystem::path &output_archive,
                    const CompressOptions &options, const FieldSeed &defaults,
                    std::span<const PathRankOverride> overrides,
                    Candidate candidate = {field_candidate, 1}, std::uint32_t catalog_revision = 1,
                    const std::stop_token &stop = {});
} // namespace seedarc::experimental::seed_study
