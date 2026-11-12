// SPDX-License-Identifier: GPL-3.0-or-later
#include "seed/strategy.hpp"

#include <charconv>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace study = seedarc::experimental::seed_study;
using seedarc::experimental::Codec;

namespace {
constexpr std::string_view usage =
    "Research plan viewer; does not compress or modify archives.\n"
    "Usage:\n"
    "  seedarc_seed_plan a-r1 SEED BLOCKS [COMPARE_SEED]\n"
    "  seedarc_seed_plan b-r1 CODEC ZSTD_RANK LZMA2_RANK BLOCKS "
    "[COMPARE_CODEC COMPARE_ZSTD_RANK COMPARE_LZMA2_RANK]\n"
    "Seed fields: 1..16 hex digits, no 0x prefix. B ranks: zstd 0..12 hex (level 1..19),\n"
    "lzma2 0..9 hex (preset 0..9). BLOCKS: decimal 0..65536. Output: JSON to stdout.\n"
    "These are revision 1 experiment fields, not an archive seed encoding.\n";

std::optional<std::uint64_t> number(std::string_view text, int base) {
    if (text.empty() || text.size() > (base == 16 ? 16U : 5U)) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, base);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::optional<study::Seed> parse_seed(bool structured, char *const *arguments) {
    const auto first = number(arguments[0], 16);
    if (!first) {
        return std::nullopt;
    }
    if (!structured) {
        return study::RandomSeed{*first};
    }
    const auto zstd = number(arguments[1], 16);
    const auto lzma = number(arguments[2], 16);
    if (!zstd || *zstd > 18 || !lzma || *lzma > 9) {
        return std::nullopt;
    }
    return study::FieldSeed{*first, static_cast<std::uint32_t>(*zstd),
                            static_cast<std::uint32_t>(*lzma)};
}

std::string hex(std::uint64_t value) {
    std::ostringstream result;
    result << std::hex << value;
    return result.str();
}

void write_seed(const study::Seed &seed) {
    if (const auto *random = std::get_if<study::RandomSeed>(&seed)) {
        std::cout << "{\"value_hex\":\"" << hex(random->value) << "\"}";
    } else {
        const auto &fields = std::get<study::FieldSeed>(seed);
        std::cout << "{\"codec_hex\":\"" << hex(fields.codec) << "\",\"zstd_rank_hex\":\""
                  << hex(fields.zstd_rank) << "\",\"lzma2_rank_hex\":\"" << hex(fields.lzma2_rank)
                  << "\"}";
    }
}

void write_plan(const study::BlockPlan &plan) {
    const auto *name = plan.codec == Codec::store  ? "store"
                       : plan.codec == Codec::zstd ? "zstd"
                                                   : "lzma2";
    std::cout << "{\"codec\":\"" << name << "\",\"level\":" << plan.level << '}';
}

int invalid() {
    std::cerr << "Invalid research plan arguments.\n" << usage;
    return 2;
}
} // namespace

int main(int argc, char *argv[]) {
    if (argc == 2 && std::string_view{argv[1]} == "--help") {
        std::cout << usage;
        return 0;
    }
    if (argc < 2) {
        return invalid();
    }
    const std::string_view candidate_name{argv[1]};
    const bool structured = candidate_name == "b-r1";
    if (!structured && candidate_name != "a-r1") {
        return invalid();
    }
    const int fields = structured ? 3 : 1;
    const int base_count = fields + 3;
    if (argc != base_count && argc != base_count + fields) {
        return invalid();
    }
    const auto seed = parse_seed(structured, argv + 2);
    const auto count = number(argv[2 + fields], 10);
    std::optional<study::Seed> compared;
    if (argc != base_count) {
        compared = parse_seed(structured, argv + base_count);
        if (!compared) {
            return invalid();
        }
    }
    if (!seed || !count || *count > 65536) {
        return invalid();
    }
    const study::Candidate candidate{structured ? study::field_candidate : study::random_candidate,
                                     1};
    std::cout << "{\"schema\":\"seedarc.seed-plan.r1\",\"plan_only\":true,\"candidate_id\":"
              << candidate.id << ",\"revision\":1,\"seed\":";
    write_seed(*seed);
    std::cout << ",\"compare_seed\":";
    if (compared) {
        write_seed(*compared);
    } else {
        std::cout << "null";
    }
    std::cout << ",\"blocks\":[";
    std::uint64_t changed = 0;
    for (std::uint64_t id = 0; id < *count; ++id) {
        const auto plan = study::make_plan(candidate, *seed, id);
        const auto other = compared ? study::make_plan(candidate, *compared, id) : plan;
        if (!plan || !other) {
            std::cerr << "Internal study plan error\n";
            return 1;
        }
        if (id != 0) {
            std::cout << ',';
        }
        std::cout << "{\"block_id\":" << id << ",\"proposed\":";
        write_plan(*plan);
        if (compared) {
            const bool differs = *plan != *other;
            changed += differs ? 1U : 0U;
            std::cout << ",\"compared\":";
            write_plan(*other);
            std::cout << ",\"changed\":" << (differs ? "true" : "false");
        }
        std::cout << '}';
    }
    std::cout << "],\"changed_blocks\":";
    if (compared) {
        std::cout << changed;
    } else {
        std::cout << "null";
    }
    std::cout << "}\n";
    return std::cout ? 0 : 1;
}
