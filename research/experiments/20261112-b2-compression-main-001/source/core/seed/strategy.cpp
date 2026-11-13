// SPDX-License-Identifier: GPL-3.0-or-later
#include "seed/strategy.hpp"
#include "splitmix64/splitmix64.hpp"

namespace seedarc::experimental::seed_study {
namespace {
// SplitMix64's explicitly specified unsigned arithmetic, used as a stateless mixer. This is
// not a cryptographic primitive. The domain constants are part of candidate revision 1.
constexpr std::uint64_t codec_domain = 0x636f6465632d7231ULL;
constexpr std::uint64_t level_domain = 0x6c6576656c2d7231ULL;

std::uint64_t mix(std::uint64_t value) noexcept { return third_party::splitmix64_step(value); }

std::uint64_t sample(std::uint64_t seed, std::uint64_t block_id, std::uint64_t domain) noexcept {
    return mix(seed ^ mix(block_id ^ domain));
}

Codec choose_codec(std::uint64_t value) noexcept {
    // Deliberately simple modulo mapping; the tiny modulo bias is documented for this study.
    switch (value % 3) {
    case 0:
        return Codec::store;
    case 1:
        return Codec::zstd;
    default:
        return Codec::lzma2;
    }
}

bool valid_plan(const BlockPlan &plan) noexcept {
    switch (plan.codec) {
    case Codec::store:
        return plan.level == 0;
    case Codec::zstd:
        return plan.level >= 1 && plan.level <= 19;
    case Codec::lzma2:
        return plan.level <= 9;
    }
    return false;
}
} // namespace

std::expected<BlockPlan, Error> make_plan(Candidate candidate, const Seed &seed,
                                          std::uint64_t block_id) {
    if (candidate.revision != 1 ||
        (candidate.id != random_candidate && candidate.id != field_candidate)) {
        return std::unexpected(
            Error{ErrorCode::unsupported_feature, "unknown seed study candidate or revision"});
    }
    BlockPlan plan{Codec::store, 0};
    if (candidate.id == random_candidate) {
        const auto *fields = std::get_if<RandomSeed>(&seed);
        if (fields == nullptr) {
            return std::unexpected(
                Error{ErrorCode::invalid_argument, "a-r1 requires a random seed"});
        }
        plan.codec = choose_codec(sample(fields->value, block_id, codec_domain));
        const auto value = sample(fields->value, block_id, level_domain);
        if (plan.codec == Codec::zstd) {
            plan.level = static_cast<std::uint32_t>(value % 19) + 1;
        } else if (plan.codec == Codec::lzma2) {
            plan.level = static_cast<std::uint32_t>(value % 10);
        }
    } else {
        const auto *fields = std::get_if<FieldSeed>(&seed);
        if (fields == nullptr || fields->zstd_rank > 18 || fields->lzma2_rank > 9) {
            return std::unexpected(
                Error{ErrorCode::invalid_argument, "b-r1 requires legal codec and level fields"});
        }
        plan.codec = choose_codec(sample(fields->codec, block_id, codec_domain));
        if (plan.codec == Codec::zstd) {
            plan.level = fields->zstd_rank + 1;
        } else if (plan.codec == Codec::lzma2) {
            plan.level = fields->lzma2_rank;
        }
    }
    if (!valid_plan(plan)) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "invalid generated block plan"});
    }
    return plan;
}
} // namespace seedarc::experimental::seed_study
