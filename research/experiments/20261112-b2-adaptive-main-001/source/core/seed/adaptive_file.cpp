// SPDX-License-Identifier: GPL-3.0-or-later
#include "seed/adaptive_file.hpp"

#include "archive_plan.hpp"
#include "codec/codec.hpp"
#include "filesystem/input_tree.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <map>

namespace seedarc::experimental::seed_study {
namespace {
struct Candidate3 {
    Codec codec;
    std::uint32_t level;
};
// Fixed trial order; also the tie-break order (earlier wins).
constexpr std::array<Candidate3, 3> trial_configs{
    {{Codec::zstd, 3}, {Codec::lzma2, 6}, {Codec::zstd, 6}}};
constexpr std::size_t zstd3 = 0, lzma6 = 1, zstd6 = 2;

Error cancelled() { return {ErrorCode::cancelled, "operation cancelled"}; }

class Trials {
  public:
    explicit Trials(std::size_t chunk) : packed_(chunk) {}

    // Output length as the core would store it, or empty when this encoder is unavailable.
    std::optional<std::uint64_t> run(std::size_t which, std::span<const std::byte> block) {
        auto &slot = encoders_[which];
        if (!slot) {
            const auto &c = trial_configs[which];
            auto created = codec::Encoder::create(static_cast<std::uint32_t>(c.codec), c.level);
            if (!created) {
                return std::nullopt;
            }
            slot.emplace(std::move(*created));
        }
        // Same rule as the writer: a raw-1 buffer, so only a strictly smaller result is kept.
        const auto size = slot->encode(block, std::span{packed_}.first(block.size() - 1));
        if (!size) {
            return std::nullopt;
        }
        return *size != 0 ? *size : block.size();
    }

  private:
    std::vector<std::byte> packed_;
    std::array<std::optional<codec::Encoder>, 3> encoders_;
};

std::uint64_t block_bytes(std::uint64_t file_bytes, std::uint64_t chunk, std::uint64_t index) {
    return (std::min)(chunk, file_bytes - index * chunk);
}

// Reads the listed blocks (ascending) sequentially; stops after the last one.
std::expected<std::vector<std::vector<std::byte>>, Error>
read_blocks(const internal::InputTree &tree, const internal::InputEntry &entry, std::uint64_t chunk,
            std::span<const std::uint64_t> wanted, std::uint64_t &read_bytes,
            const std::stop_token &stop) {
    auto file = tree.open_file(entry);
    if (!file) {
        return std::unexpected(file.error());
    }
    std::vector<std::vector<std::byte>> blocks;
    std::vector<std::byte> buffer(static_cast<std::size_t>(chunk));
    std::size_t next = 0;
    for (std::uint64_t index = 0; next < wanted.size(); ++index) {
        if (stop.stop_requested()) {
            return std::unexpected(cancelled());
        }
        const auto want = static_cast<std::size_t>(block_bytes(entry.size, chunk, index));
        const auto chunk_view = std::span{buffer}.first(want);
        const auto got = file->read(chunk_view);
        if (!got) {
            return std::unexpected(got.error());
        }
        if (*got != want) {
            return std::unexpected(
                Error{ErrorCode::io_error, "input changed during selection: " + entry.path});
        }
        read_bytes += want;
        if (index == wanted[next]) {
            blocks.emplace_back(chunk_view.begin(), chunk_view.end());
            ++next;
        }
    }
    return blocks;
}

bool qualifies(std::uint64_t baseline, std::uint64_t candidate, std::uint64_t sample_bytes,
               std::uint64_t file_bytes, const SelectorConfig &config) {
    if (candidate >= baseline) {
        return false;
    }
    const auto gain = baseline - candidate;
    if (gain * 1000 < baseline * config.min_gain_permille) {
        return false;
    }
    // Extrapolated saving gain * file / sample >= min_gain_bytes, without overflow.
    if (file_bytes > std::numeric_limits<std::uint64_t>::max() / gain) {
        return true;
    }
    return gain * file_bytes >= config.min_gain_bytes * sample_bytes;
}

class Selector {
  public:
    Selector(const internal::InputTree &tree, const SelectorConfig &config, std::uint64_t chunk,
             const std::stop_token &stop)
        : tree_(tree), config_(config), chunk_(chunk), stop_(stop),
          trials_(static_cast<std::size_t>(chunk)) {
        const bool oracle = config.kind == SelectorKind::oracle;
        report.selector_version = oracle ? oracle_selector_version : adaptive_selector_version;
        if (!oracle && config.trial_budget_bytes != unbounded_trial_budget) {
            report.budget_limit = config.trial_budget_bytes;
        }
    }

    std::expected<void, Error> decide(FileDecision &d, const internal::InputEntry &entry) {
        if (d.file_bytes == 0) {
            d.reason = "empty_file";
            return {};
        }
        return config_.kind == SelectorKind::oracle ? oracle(d, entry) : adaptive(d, entry);
    }

    SelectorReport report;

  private:
    bool affordable(std::uint64_t bytes) const {
        return !report.budget_limit || bytes <= *report.budget_limit - report.budget_used;
    }

    // One trial; false means the encoder failed (the caller falls back deterministically).
    bool trial(FileDecision &d, std::size_t which, std::uint64_t index,
               std::span<const std::byte> block, std::uint64_t &sum) {
        const auto out = trials_.run(which, block);
        report.budget_used += block.size();
        ++report.trial_count;
        if (!out) {
            return false;
        }
        const auto &c = trial_configs[which];
        d.trials.push_back({index, c.codec, c.level, block.size(), *out, *out < block.size()});
        sum += *out;
        return true;
    }

    std::expected<void, Error> adaptive(FileDecision &d, const internal::InputEntry &entry) {
        if (d.file_bytes < config_.min_file_bytes) {
            d.reason = "below_min_size";
            return {};
        }
        const auto count = (d.file_bytes + chunk_ - 1) / chunk_;
        std::vector<std::uint64_t> wanted{0, count / 2, count - 1};
        std::ranges::sort(wanted);
        wanted.erase(std::ranges::unique(wanted).begin(), wanted.end());
        std::uint64_t sample = 0;
        for (const auto index : wanted) {
            sample += block_bytes(d.file_bytes, chunk_, index);
        }
        if (!affordable(2 * sample)) {
            d.reason = "budget_exhausted";
            ++report.budget_exhausted_files;
            return {};
        }
        auto blocks = read_blocks(tree_, entry, chunk_, wanted, d.read_bytes, stop_);
        if (!blocks) {
            return std::unexpected(blocks.error());
        }
        d.sampled_blocks = wanted;
        std::array<std::uint64_t, 3> sums{};
        for (const std::size_t which : {zstd3, lzma6}) {
            for (std::size_t i = 0; i < wanted.size(); ++i) {
                if (!trial(d, which, wanted[i], (*blocks)[i], sums[which])) {
                    d.reason = "trial_failed";
                    return {};
                }
            }
        }
        if (sums[zstd3] == sample && sums[lzma6] == sample) {
            d.reason = "no_gain";
            return {};
        }
        bool tried_zstd6 = false;
        if ((sample - sums[zstd3]) * 1000 >= sample * config_.min_gain_permille) {
            if (affordable(sample)) {
                for (std::size_t i = 0; i < wanted.size(); ++i) {
                    if (!trial(d, zstd6, wanted[i], (*blocks)[i], sums[zstd6])) {
                        d.reason = "trial_failed";
                        return {};
                    }
                }
                tried_zstd6 = true;
            } else {
                d.notes.emplace_back("zstd6_skipped_budget");
            }
        }
        std::optional<std::size_t> best;
        for (const std::size_t which : {lzma6, zstd6}) {
            if ((which == zstd6 && !tried_zstd6) ||
                !qualifies(sums[zstd3], sums[which], sample, d.file_bytes, config_)) {
                continue;
            }
            if (!best || sums[which] < sums[*best]) {
                best = which;
            }
        }
        if (!best) {
            d.reason = "insufficient_gain";
            return {};
        }
        d.codec = trial_configs[*best].codec;
        d.level = trial_configs[*best].level;
        d.reason = *best == lzma6 ? "lzma2_6_saves" : "zstd_6_saves";
        return {};
    }

    std::expected<void, Error> oracle(FileDecision &d, const internal::InputEntry &entry) {
        const auto count = (d.file_bytes + chunk_ - 1) / chunk_;
        std::array<std::uint64_t, 3> sums{};
        auto file = tree_.open_file(entry);
        if (!file) {
            return std::unexpected(file.error());
        }
        std::vector<std::byte> buffer(static_cast<std::size_t>(chunk_));
        for (std::uint64_t index = 0; index < count; ++index) {
            if (stop_.stop_requested()) {
                return std::unexpected(cancelled());
            }
            const auto want = static_cast<std::size_t>(block_bytes(d.file_bytes, chunk_, index));
            const auto block = std::span{buffer}.first(want);
            const auto got = file->read(block);
            if (!got) {
                return std::unexpected(got.error());
            }
            if (*got != want) {
                return std::unexpected(
                    Error{ErrorCode::io_error, "input changed during selection: " + entry.path});
            }
            d.read_bytes += want;
            d.sampled_blocks.push_back(index);
            for (std::size_t which = 0; which < trial_configs.size(); ++which) {
                if (!trial(d, which, index, block, sums[which])) {
                    d.reason = "trial_failed";
                    return {};
                }
            }
        }
        // B2-r4 B2-file-codec rule: LZMA2-6 if smaller, else Zstd-3 with a 3->6 override.
        if (sums[lzma6] < sums[zstd3]) {
            d.codec = Codec::lzma2;
            d.level = 6;
            d.reason = "oracle_lzma2_6";
        } else if (sums[zstd6] < sums[zstd3]) {
            d.level = 6;
            d.reason = "oracle_zstd_6";
        } else {
            d.reason = "oracle_zstd_3";
        }
        return {};
    }

    const internal::InputTree &tree_;
    const SelectorConfig &config_;
    std::uint64_t chunk_;
    const std::stop_token &stop_;
    Trials trials_;
};

double now_ms() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
} // namespace

std::expected<SelectedCompression, Error> compress_selected_archive(
    const std::filesystem::path &input, const std::filesystem::path &output_archive,
    const CompressOptions &options, const SelectorConfig &config, const std::stop_token &stop) {
    if (config.min_gain_permille > 1000 || config.min_file_bytes == 0) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "invalid selector parameters"});
    }
    SelectedCompression result;
    const internal::PlanBuilder builder =
        [&](std::span<const IndexEntry> entries,
            std::uint64_t count) -> std::expected<std::vector<internal::BlockEncoding>, Error> {
        if (count > max_scope_blocks) {
            return std::unexpected(Error{ErrorCode::resource_limit, "selector block limit"});
        }
        const auto wall_start = now_ms();
        const auto cpu_start = config.cpu_ms ? std::optional{config.cpu_ms()} : std::nullopt;
        // A second enumeration through the same safe reader; only manifest paths are used, so
        // the writer's own temporary (if inside the input) is never sampled.
        const auto tree = internal::InputTree::open(input);
        if (!tree) {
            return std::unexpected(tree.error());
        }
        const auto listed = tree->enumerate(options.limits, nullptr, stop);
        if (!listed) {
            return std::unexpected(listed.error());
        }
        std::map<std::string_view, const internal::InputEntry *> by_path;
        for (const auto &entry : *listed) {
            by_path.emplace(entry.path, &entry);
        }
        Selector selector(*tree, config, options.chunk_size_bytes, stop);
        std::vector<internal::BlockEncoding> plan;
        plan.reserve(static_cast<std::size_t>(count));
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const auto &entry = entries[i];
            if (entry.directory) {
                continue;
            }
            if (result.files.size() == max_scope_files) {
                return std::unexpected(Error{ErrorCode::resource_limit, "selector file limit"});
            }
            const auto found = by_path.find(entry.path);
            if (found == by_path.end() || found->second->directory ||
                found->second->size != entry.size) {
                return std::unexpected(
                    Error{ErrorCode::io_error, "input changed during selection: " + entry.path});
            }
            result.files.push_back({i, entry.path});
            FileDecision decision;
            decision.file_id = i;
            decision.file_key = entry.path;
            decision.file_bytes = entry.size;
            const auto file_wall = now_ms();
            const auto file_cpu = config.cpu_ms ? std::optional{config.cpu_ms()} : std::nullopt;
            if (auto decided = selector.decide(decision, *found->second); !decided) {
                return std::unexpected(decided.error());
            }
            decision.budget_used_after = selector.report.budget_used;
            decision.wall_ms = now_ms() - file_wall;
            if (file_cpu) {
                decision.cpu_ms = config.cpu_ms() - *file_cpu;
            }
            selector.report.read_bytes += decision.read_bytes;
            for (std::uint64_t local = 0; local < entry.block_count; ++local) {
                result.planned.push_back(
                    {{i, local, entry.first_block + local}, {decision.codec, decision.level}});
                plan.push_back({decision.codec, decision.level});
            }
            selector.report.files.push_back(std::move(decision));
        }
        selector.report.wall_ms = now_ms() - wall_start;
        if (cpu_start) {
            selector.report.cpu_ms = config.cpu_ms() - *cpu_start;
        }
        result.report = std::move(selector.report);
        return plan;
    };
    const auto summary =
        internal::compress_archive_planned(input, output_archive, options, builder, stop);
    if (!summary) {
        return std::unexpected(summary.error());
    }
    result.summary = *summary;
    return result;
}
} // namespace seedarc::experimental::seed_study
