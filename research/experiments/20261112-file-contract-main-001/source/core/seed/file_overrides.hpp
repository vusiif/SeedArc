// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "strategy.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace seedarc::experimental::seed_study {
inline constexpr std::size_t max_scope_blocks = 65536;
inline constexpr std::size_t max_scope_files = 4096;
inline constexpr std::size_t max_file_overrides = 4096;

struct BlockIdentity {
    std::uint64_t file_id;
    std::uint64_t file_block_index;
    std::uint64_t global_block_id;
    bool operator==(const BlockIdentity &) const = default;
};

struct FileRankOverride {
    std::uint64_t file_id;
    std::optional<std::uint32_t> zstd_rank;
    std::optional<std::uint32_t> lzma2_rank;
};

struct PlannedBlock {
    BlockIdentity identity;
    BlockPlan plan;
    bool operator==(const PlannedBlock &) const = default;
};

// A separate, bounded editing overlay over B-r1, not a new compact seed representation.
// File IDs are meaningful only in this fixed list. Each global ID and (file ID, local index)
// must be unique. Output is sorted by global ID, independent of input/override order.
// Missing fields inherit defaults; remove a field/entry to clear it. Empty entries are invalid.
[[nodiscard]] std::expected<std::vector<PlannedBlock>, Error>
make_file_plans(const FieldSeed &defaults, std::span<const BlockIdentity> blocks,
                std::span<const FileRankOverride> overrides, std::uint32_t overlay_revision = 1);
} // namespace seedarc::experimental::seed_study
