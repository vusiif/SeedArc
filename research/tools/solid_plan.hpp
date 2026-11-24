// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "append_common.hpp"

namespace seedarc::research_tools::append {
// Experimental solid-plan-r1, seed mapping revision 2. Zstd-9 solid frames;
// low two bits select the existing grouping rule, next three encode k - 1.
// No dictionary/training fields: those belong to the older block-plan mapping.
inline constexpr std::uint16_t solid_seed_revision = 2;
inline constexpr std::uint8_t solid_max_groups = 8;

inline bool valid_solid_plan(const Plan &p) {
    return p.rule < 4 && p.k >= 1 && p.k <= solid_max_groups && p.k <= max_groups &&
           canonical(p) == p &&
           std::ranges::all_of(
               p.d, [](auto v) { return v == 0; }) &&
           std::ranges::all_of(p.t, [](auto v) { return v == 0; });
}
inline std::optional<Plan> decode_solid_seed(std::uint32_t seed) {
    if (seed > 31) {
        return std::nullopt;
    }
    Plan p;
    p.rule = static_cast<std::uint8_t>(seed & 3);
    p.k = static_cast<std::uint8_t>((seed >> 2) + 1);
    return valid_solid_plan(p) ? std::optional{p} : std::nullopt;
}
inline std::uint32_t encode_solid_seed(const Plan &p) {
    return p.rule | (static_cast<std::uint32_t>(p.k - 1) << 2);
}
// Concrete descriptor bytes: solid layout ID 1, grouping rule, k, Zstd level 9.
inline std::uint32_t encode_solid_explicit(const Plan &p) {
    return 1U | (std::uint32_t{p.rule} << 8) | (std::uint32_t{p.k} << 16) | (9U << 24);
}
inline std::optional<Plan> decode_solid_explicit(std::uint32_t value) {
    if ((value & 255) != 1 || (value >> 24) != 9) {
        return std::nullopt;
    }
    Plan p;
    p.rule = static_cast<std::uint8_t>(value >> 8);
    p.k = static_cast<std::uint8_t>(value >> 16);
    return valid_solid_plan(p) ? std::optional{p} : std::nullopt;
}
// A direct concrete-plan interface, independent of the Seed representation.
inline std::optional<Plan> parse_solid_explicit(std::string_view text) {
    const auto separator = text.find(':');
    if (separator == std::string_view::npos) {
        return std::nullopt;
    }
    const auto rule = number(text.substr(0, separator));
    const auto groups = number(text.substr(separator + 1));
    if (!rule || !groups || *rule >= 4 || *groups < 1 || *groups > solid_max_groups) {
        return std::nullopt;
    }
    Plan p;
    p.rule = static_cast<std::uint8_t>(*rule);
    p.k = static_cast<std::uint8_t>(*groups);
    return valid_solid_plan(p) ? std::optional{p} : std::nullopt;
}
inline std::vector<Plan> solid_candidates() {
    std::vector<Plan> result;
    for (std::uint8_t groups = 1; groups <= solid_max_groups; ++groups) {
        for (std::uint8_t rule = 0; rule < 4; ++rule) {
            Plan p;
            p.rule = rule;
            p.k = groups;
            if (valid_solid_plan(p)) {
                result.push_back(p);
            }
        }
    }
    return result;
}
} // namespace seedarc::research_tools::append
