// SPDX-License-Identifier: GPL-3.0-or-later
#include "seed/strategy.hpp"

#include <array>
#include <iostream>
#include <limits>
#include <string_view>

namespace study = seedarc::experimental::seed_study;
using seedarc::ErrorCode;
using seedarc::experimental::Codec;

namespace {
int failures = 0;
void check(bool condition, std::string_view message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

bool valid(const study::BlockPlan &plan) {
    return plan.codec == Codec::store  ? plan.level == 0
           : plan.codec == Codec::zstd ? plan.level >= 1 && plan.level <= 19
                                       : plan.codec == Codec::lzma2 && plan.level <= 9;
}
} // namespace

int main() {
    constexpr study::Candidate a{study::random_candidate, 1};
    constexpr study::Candidate b{study::field_candidate, 1};
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    struct Vector {
        std::uint64_t seed;
        std::uint64_t block;
        study::BlockPlan expected;
    };
    constexpr std::array vectors{Vector{0, 0, {Codec::lzma2, 2}},
                                 Vector{0, 1, {Codec::zstd, 2}},
                                 Vector{0, 2, {Codec::store, 0}},
                                 Vector{0, 3, {Codec::zstd, 9}},
                                 Vector{1, 0, {Codec::store, 0}},
                                 Vector{maximum, maximum, {Codec::store, 0}},
                                 Vector{0x0123456789abcdefULL, 42, {Codec::store, 0}},
                                 Vector{maximum, 0, {Codec::zstd, 2}}};
    for (const auto &item : vectors) {
        const auto actual = study::make_plan(a, study::RandomSeed{item.seed}, item.block);
        check(actual && *actual == item.expected, "A fixed vector");
    }
    constexpr std::array expected_b{
        study::BlockPlan{Codec::lzma2, 6}, study::BlockPlan{Codec::zstd, 3},
        study::BlockPlan{Codec::store, 0}, study::BlockPlan{Codec::zstd, 3}};
    for (std::size_t id = 0; id < expected_b.size(); ++id) {
        const auto actual = study::make_plan(b, study::FieldSeed{0, 2, 6}, id);
        check(actual && *actual == expected_b[id], "B fixed vector");
    }
    const auto reject = [](study::Candidate candidate, const study::Seed &seed, ErrorCode code) {
        const auto result = study::make_plan(candidate, seed, 0);
        check(!result && result.error().code == code, "invalid candidate, revision or fields");
    };
    reject({0, 1}, study::RandomSeed{0}, ErrorCode::unsupported_feature);
    reject({study::random_candidate, 0}, study::RandomSeed{0}, ErrorCode::unsupported_feature);
    reject({study::field_candidate, 2}, study::FieldSeed{0, 0, 0}, ErrorCode::unsupported_feature);
    reject(a, study::FieldSeed{0, 0, 0}, ErrorCode::invalid_argument);
    reject(b, study::RandomSeed{0}, ErrorCode::invalid_argument);
    reject(b, study::FieldSeed{0, 19, 0}, ErrorCode::invalid_argument);
    reject(b, study::FieldSeed{0, 0, 10}, ErrorCode::invalid_argument);
    reject(b, study::FieldSeed{0, UINT32_MAX, UINT32_MAX}, ErrorCode::invalid_argument);

    // Forward and reverse/random access must produce the same plan without shared PRNG state.
    std::array<study::BlockPlan, 256> saved{};
    for (const auto candidate : {a, b}) {
        const study::Seed seed = candidate.id == study::random_candidate
                                     ? study::Seed{study::RandomSeed{maximum}}
                                     : study::Seed{study::FieldSeed{maximum, 18, 9}};
        for (std::size_t i = 0; i < saved.size(); ++i) {
            const auto plan = study::make_plan(candidate, seed, i);
            check(plan && valid(*plan), "legal plan for every block");
            if (plan) {
                saved[i] = *plan;
            }
        }
        for (std::size_t i = saved.size(); i-- > 0;) {
            const auto plan = study::make_plan(candidate, seed, i);
            check(plan && *plan == saved[i], "generation order independence");
        }
        const auto edge = study::make_plan(candidate, seed, maximum);
        check(edge && valid(*edge), "maximum block ordinal is legal");
    }

    // Sweep every legal B level; changing one field cannot change other responsibilities.
    std::array<bool, 3> seen{};
    for (std::uint64_t id = 0; id < 256; ++id) {
        const auto base = study::make_plan(b, study::FieldSeed{0, 0, 0}, id);
        check(base.has_value(), "B base plan exists");
        if (!base) {
            continue;
        }
        seen[static_cast<std::size_t>(base->codec)] = true;
        for (std::uint32_t rank = 0; rank <= 18; ++rank) {
            const auto plan = study::make_plan(b, study::FieldSeed{0, rank, 0}, id);
            check(plan && plan->codec == base->codec &&
                      plan->level == (plan->codec == Codec::zstd ? rank + 1 : base->level),
                  "Zstd rank isolation and adjacent levels");
        }
        for (std::uint32_t rank = 0; rank <= 9; ++rank) {
            const auto plan = study::make_plan(b, study::FieldSeed{0, 0, rank}, id);
            check(plan && plan->codec == base->codec &&
                      plan->level == (plan->codec == Codec::lzma2 ? rank : base->level),
                  "LZMA2 rank isolation and adjacent presets");
        }
    }
    check(seen[0] && seen[1] && seen[2], "isolation sweep covers all three codecs");
    std::cout << (failures == 0 ? "all seed candidate checks passed" : "seed checks failed")
              << '\n';
    return failures == 0 ? 0 : 1;
}
