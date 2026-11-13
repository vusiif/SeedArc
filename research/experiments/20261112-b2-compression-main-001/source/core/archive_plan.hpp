// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "seedarc/archive_index.hpp"
#include "seedarc/store_archive.hpp"

#include <functional>
#include <vector>

namespace seedarc::experimental::internal {
struct BlockEncoding {
    Codec codec;
    std::uint32_t config;
};

// Trusted in-process planner, called once with the writer's actual canonical manifest.
// Views expire on return. Return one concrete encoding per global block ordinal.
// No seed interpretation or planner state is serialized into the archive.
using PlanBuilder = std::function<std::expected<std::vector<BlockEncoding>, Error>(
    std::span<const IndexEntry>, std::uint64_t)>;

// Internal experimental seam. options.codec/level must retain Store/unset defaults.
// The complete plan is validated before reading file content; failure rolls back the output.
[[nodiscard]] std::expected<ArchiveSummary, Error> compress_archive_planned(
    const std::filesystem::path &input, const std::filesystem::path &output_archive,
    const CompressOptions &options, const PlanBuilder &builder, const std::stop_token &stop = {});
} // namespace seedarc::experimental::internal
