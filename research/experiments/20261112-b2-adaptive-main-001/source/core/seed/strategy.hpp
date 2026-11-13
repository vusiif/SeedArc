// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Internal, replaceable research candidates; not a seed wire format or an archive contract.
#include "seedarc/error.hpp"
#include "seedarc/store_archive.hpp"

#include <cstdint>
#include <expected>
#include <variant>

namespace seedarc::experimental::seed_study {
inline constexpr std::uint32_t random_candidate = 1;
inline constexpr std::uint32_t field_candidate = 2;

struct Candidate {
    std::uint32_t id;
    std::uint32_t revision;
};

struct RandomSeed {
    std::uint64_t value;
};

struct FieldSeed {
    std::uint64_t codec;
    std::uint32_t zstd_rank;  // 0..18 maps directly to level 1..19
    std::uint32_t lzma2_rank; // 0..9 maps directly to preset 0..9
};

using Seed = std::variant<RandomSeed, FieldSeed>;

struct BlockPlan {
    Codec codec;
    std::uint32_t level; // Store always uses 0; this is a proposed, not an executed, plan.
    bool operator==(const BlockPlan &) const = default;
};

// r1 identifies a block by its global ordinal in a fixed canonical block list. Inserting a
// preceding block changes later identities. Calls are independent of traversal order.
[[nodiscard]] std::expected<BlockPlan, Error> make_plan(Candidate candidate, const Seed &seed,
                                                        std::uint64_t block_id);
} // namespace seedarc::experimental::seed_study
