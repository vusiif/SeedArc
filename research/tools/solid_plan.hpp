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
// ---- solid-recipe-r1, seed mapping revision 3 (ADR-0006) --------------------------------------
// A recipe names the whole solid write strategy: grouping rule and k (as revision 2), the frame
// limit step, the codec and its level. r1 accepts only Zstd 9; other codec / level values are
// reserved and rejected, so a later revision only turns invalid values into valid ones.
inline constexpr std::uint16_t solid_recipe_revision = 3;
inline constexpr std::array<std::uint64_t, 6> recipe_frame_limits{
    std::uint64_t{1} << 30, 16ULL << 20, 32ULL << 20, 64ULL << 20, 128ULL << 20, 256ULL << 20};
struct RecipeFields {
    std::uint8_t frame_code = 0; // index into recipe_frame_limits; 0 = whole (1 GiB bound)
    std::uint8_t codec = 0;      // 0 = Zstd
    std::uint8_t level = 9;
    bool operator==(const RecipeFields &) const = default;
};
struct SolidRecipe {
    Plan plan;
    RecipeFields fields;
};
inline bool valid_recipe(const SolidRecipe &r) {
    return valid_solid_plan(r.plan) && r.fields.frame_code < recipe_frame_limits.size() &&
           r.fields.codec == 0 && r.fields.level == 9;
}
inline std::uint64_t recipe_frame_limit(const RecipeFields &f) {
    return recipe_frame_limits[f.frame_code];
}
// bits 0-4 as revision 2 | 5-7 frame code | 8-9 codec | 10-14 level | 15-31 zero
inline std::uint32_t encode_recipe_seed(const SolidRecipe &r) {
    return encode_solid_seed(r.plan) | (std::uint32_t{r.fields.frame_code} << 5) |
           (std::uint32_t{r.fields.codec} << 8) | (std::uint32_t{r.fields.level} << 10);
}
inline std::optional<SolidRecipe> decode_recipe_seed(std::uint32_t value) {
    if ((value >> 15) != 0) {
        return std::nullopt;
    }
    const auto plan = decode_solid_seed(value & 31);
    if (!plan) {
        return std::nullopt;
    }
    const SolidRecipe r{*plan, {static_cast<std::uint8_t>((value >> 5) & 7),
                                static_cast<std::uint8_t>((value >> 8) & 3),
                                static_cast<std::uint8_t>((value >> 10) & 31)}};
    return valid_recipe(r) ? std::optional{r} : std::nullopt;
}
// Concrete descriptor (two words): layout ID 2, rule, k, level | frame code, codec.
inline std::array<std::uint32_t, 2> encode_recipe_explicit(const SolidRecipe &r) {
    return {2U | (std::uint32_t{r.plan.rule} << 8) | (std::uint32_t{r.plan.k} << 16) |
                (std::uint32_t{r.fields.level} << 24),
            std::uint32_t{r.fields.frame_code} | (std::uint32_t{r.fields.codec} << 8)};
}
inline std::optional<SolidRecipe> decode_recipe_explicit(std::uint32_t first,
                                                         std::uint32_t second) {
    if ((first & 255) != 2 || (second >> 16) != 0) {
        return std::nullopt;
    }
    SolidRecipe r;
    r.plan.rule = static_cast<std::uint8_t>(first >> 8);
    r.plan.k = static_cast<std::uint8_t>(first >> 16);
    r.fields = {static_cast<std::uint8_t>(second & 255), static_cast<std::uint8_t>(second >> 8),
                static_cast<std::uint8_t>(first >> 24)};
    return valid_recipe(r) ? std::optional{r} : std::nullopt;
}
// Canonical readable form, e.g. solid/zstd-9/g1-r0/frame-32MiB (frame-whole for code 0).
inline std::string recipe_text(const SolidRecipe &r) {
    const auto frame = r.fields.frame_code == 0
                           ? std::string{"whole"}
                           : std::to_string(recipe_frame_limit(r.fields) >> 20) + "MiB";
    return "solid/zstd-" + std::to_string(r.fields.level) + "/g" + std::to_string(r.plan.k) +
           "-r" + std::to_string(r.plan.rule) + "/frame-" + frame;
}
inline std::vector<SolidRecipe> recipe_candidates();
// Accepts the canonical text form or a revision-3 Seed value in hex ("0x...").
inline std::optional<SolidRecipe> parse_recipe(std::string_view text) {
    if (text.starts_with("0x")) {
        const auto bits = hex32(text);
        return bits ? decode_recipe_seed(*bits) : std::nullopt;
    }
    for (const auto &r : recipe_candidates()) {
        if (recipe_text(r) == text) {
            return r;
        }
    }
    return std::nullopt;
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
inline std::vector<SolidRecipe> recipe_candidates() {
    std::vector<SolidRecipe> result;
    for (const auto &plan : solid_candidates()) {
        for (std::uint8_t code = 0; code < recipe_frame_limits.size(); ++code) {
            result.push_back({plan, {code, 0, 9}});
        }
    }
    return result;
}
} // namespace seedarc::research_tools::append
