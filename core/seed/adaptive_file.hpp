// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "path_overrides.hpp"

#include <functional>
#include <limits>
#include <optional>
#include <string_view>

namespace seedarc::experimental::seed_study {
// B2-r5 encode-side file-level selectors. They only emit concrete per-block encodings for the
// existing planned-compression seam: no archive field, no seed, decoding never replays them.
inline constexpr std::string_view adaptive_selector_version = "adaptive-file-r1";
inline constexpr std::string_view oracle_selector_version = "oracle-file-r1";
inline constexpr std::uint64_t unbounded_trial_budget = std::numeric_limits<std::uint64_t>::max();

enum class SelectorKind { adaptive, oracle };

struct SelectorConfig {
    SelectorKind kind = SelectorKind::adaptive;
    // Adaptive: total raw bytes fed to trial encoders over the whole archive.
    // unbounded_trial_budget means no limit. Oracle ignores it (always unbounded).
    std::uint64_t trial_budget_bytes = 0;
    std::uint64_t min_file_bytes = 16384;
    std::uint32_t min_gain_permille = 20;
    std::uint64_t min_gain_bytes = 64;
    // Optional monotonic CPU clock in milliseconds (e.g. thread CPU time); empty: not measured.
    std::function<double()> cpu_ms;
};

struct TrialRecord {
    std::uint64_t file_block_index;
    Codec codec;
    std::uint32_t level;
    std::uint64_t input_bytes;
    std::uint64_t output_bytes; // what the core would store: encoded length, or raw if not smaller
    bool encoded;               // strictly smaller than the raw block
};

struct FileDecision {
    std::uint64_t file_id = 0;
    std::string file_key;
    std::uint64_t file_bytes = 0;
    Codec codec = Codec::zstd;
    std::uint32_t level = 3;
    std::string reason;
    std::vector<std::string> notes;
    std::vector<std::uint64_t> sampled_blocks;
    std::uint64_t read_bytes = 0;
    std::vector<TrialRecord> trials;
    std::uint64_t budget_used_after = 0;
    double wall_ms = 0;
    std::optional<double> cpu_ms;
};

struct SelectorReport {
    std::string_view selector_version;
    std::optional<std::uint64_t> budget_limit; // empty: unbounded
    std::uint64_t budget_used = 0;
    std::uint64_t trial_count = 0;
    std::uint64_t read_bytes = 0;
    std::uint64_t budget_exhausted_files = 0;
    std::vector<FileDecision> files;
    double wall_ms = 0;
    std::optional<double> cpu_ms;
};

struct SelectedCompression {
    ArchiveSummary summary;
    SelectorReport report;
    std::vector<PlanFile> files; // file_id is the manifest entry index, not stable identity
    std::vector<PlannedBlock> planned;
};

// Selection runs inside the planner callback on the writer's own validated manifest. Sampled
// content is read through the core's InputTree (no reparse points followed); the real writer
// re-reads and checks every file, so content changing after selection cannot corrupt the
// archive, it only makes the decision stale. Trial encoder failures fall back to Zstd-3.
[[nodiscard]] std::expected<SelectedCompression, Error> compress_selected_archive(
    const std::filesystem::path &input, const std::filesystem::path &output_archive,
    const CompressOptions &options, const SelectorConfig &config, const std::stop_token &stop = {});
} // namespace seedarc::experimental::seed_study
