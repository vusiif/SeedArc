// SPDX-License-Identifier: GPL-3.0-or-later
#include "seed/archive_study.hpp"

#include "archive_plan.hpp"

namespace seedarc::experimental::seed_study {
std::expected<B2Compression, Error>
compress_b2_archive(const std::filesystem::path &input, const std::filesystem::path &output_archive,
                    const CompressOptions &options, const FieldSeed &defaults,
                    std::span<const PathRankOverride> overrides, Candidate candidate,
                    std::uint32_t catalog_revision, const std::stop_token &stop) {
    if (candidate.id != field_candidate || candidate.revision != 1 || catalog_revision != 1) {
        return std::unexpected(Error{ErrorCode::unsupported_feature, "unsupported B2 study rules"});
    }
    B2Compression report;
    const internal::PlanBuilder builder =
        [&](std::span<const IndexEntry> entries,
            std::uint64_t count) -> std::expected<std::vector<internal::BlockEncoding>, Error> {
        if (count > max_scope_blocks || overrides.size() > max_file_overrides) {
            return std::unexpected(Error{ErrorCode::resource_limit, "B2 study plan limit"});
        }
        std::vector<BlockIdentity> blocks;
        blocks.reserve(static_cast<std::size_t>(count));
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const auto &entry = entries[i];
            if (entry.directory) {
                continue;
            }
            if (report.files.size() == max_scope_files) {
                return std::unexpected(Error{ErrorCode::resource_limit, "B2 study file limit"});
            }
            report.files.push_back({i, entry.path});
            for (std::uint64_t local = 0; local < entry.block_count; ++local) {
                blocks.push_back({i, local, entry.first_block + local});
            }
        }
        auto planned =
            make_path_file_plans(defaults, report.files, blocks, overrides, catalog_revision);
        if (!planned) {
            return std::unexpected(planned.error());
        }
        report.planned = std::move(*planned);
        std::vector<internal::BlockEncoding> result;
        result.reserve(report.planned.size());
        for (const auto &block : report.planned) {
            result.push_back({block.plan.codec, block.plan.level});
        }
        return result;
    };
    const auto result =
        internal::compress_archive_planned(input, output_archive, options, builder, stop);
    if (!result) {
        return std::unexpected(result.error());
    }
    report.summary = *result;
    return report;
}
} // namespace seedarc::experimental::seed_study
