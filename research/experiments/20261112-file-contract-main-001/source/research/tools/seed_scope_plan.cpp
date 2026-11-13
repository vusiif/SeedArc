// SPDX-License-Identifier: GPL-3.0-or-later
#include "seed/file_overrides.hpp"

#include <array>
#include <charconv>
#include <iostream>
#include <optional>
#include <string_view>

namespace study = seedarc::experimental::seed_study;
using seedarc::experimental::Codec;

namespace {
constexpr std::string_view usage =
    "File rank overlay study; generates plans only, no file or archive I/O.\n"
    "Usage: seedarc_seed_scope_plan overlay-r1 FILE_ID ZSTD_RANK|- LZMA2_RANK|-\n"
    "All numbers are decimal. FILE_ID: 0..2 in the fixed 3-file/12-block fixture.\n"
    "ZSTD_RANK: 0..18 (level rank+1); LZMA2_RANK: 0..9 (preset rank).\n"
    "'-' inherits that field. At least one rank is required. Removing the override\n"
    "restores defaults. Base: B-r1 codec=0, Zstd rank=2, LZMA2 rank=6.\n"
    "Example: seedarc_seed_scope_plan overlay-r1 1 3 -\n"
    "JSON compares base, global rank edit, selected file rank edit, and cleared edit.\n"
    "This is an editing overlay experiment, not a new compact seed encoding.\n";

std::optional<std::uint64_t> decimal(std::string_view text) {
    if (text.empty() || text.size() > 20) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

void optional_rank(const std::optional<std::uint32_t> &rank) {
    if (rank) {
        std::cout << *rank;
    } else {
        std::cout << "null";
    }
}

void write_defaults(const study::FieldSeed &fields) {
    // The fixture deliberately fixes codec=0; ranks are decimal semantic values.
    std::cout << "{\"codec_hex\":\"0\",\"zstd_rank\":" << fields.zstd_rank
              << ",\"lzma2_rank\":" << fields.lzma2_rank << '}';
}

void write_plan(const study::BlockPlan &plan) {
    const auto *name = plan.codec == Codec::store  ? "store"
                       : plan.codec == Codec::zstd ? "zstd"
                                                   : "lzma2";
    std::cout << "{\"codec\":\"" << name << "\",\"level\":" << plan.level << '}';
}

void write_changes(const std::vector<study::PlannedBlock> &before,
                   const std::vector<study::PlannedBlock> &after) {
    std::cout << '[';
    bool first = true;
    for (std::size_t i = 0; i < before.size(); ++i) {
        if (before[i].plan != after[i].plan) {
            if (!first) {
                std::cout << ',';
            }
            first = false;
            std::cout << before[i].identity.global_block_id;
        }
    }
    std::cout << ']';
}

int invalid() {
    std::cerr << "Invalid file overlay arguments.\n" << usage;
    return 2;
}
} // namespace

int main(int argc, char *argv[]) {
    if (argc == 2 && std::string_view{argv[1]} == "--help") {
        std::cout << usage;
        return 0;
    }
    if (argc != 5 || std::string_view{argv[1]} != "overlay-r1") {
        return invalid();
    }
    const auto file_id = decimal(argv[2]);
    if (!file_id) {
        return invalid();
    }
    study::FileRankOverride entry{*file_id, std::nullopt, std::nullopt};
    for (int index = 3; index <= 4; ++index) {
        if (std::string_view{argv[index]} == "-") {
            continue;
        }
        const auto rank = decimal(argv[index]);
        if (!rank || *rank > (index == 3 ? 18U : 9U)) {
            return invalid();
        }
        (index == 3 ? entry.zstd_rank : entry.lzma2_rank) = static_cast<std::uint32_t>(*rank);
    }

    std::array<study::BlockIdentity, 12> blocks{};
    for (std::uint64_t id = 0; id < blocks.size(); ++id) {
        blocks[static_cast<std::size_t>(id)] = {id / 4, id % 4, id};
    }
    constexpr study::FieldSeed defaults{0, 2, 6};
    auto edited_defaults = defaults;
    edited_defaults.zstd_rank = entry.zstd_rank.value_or(defaults.zstd_rank);
    edited_defaults.lzma2_rank = entry.lzma2_rank.value_or(defaults.lzma2_rank);
    const std::array entries{entry};
    const auto base = study::make_file_plans(defaults, blocks, {});
    const auto global_edit = study::make_file_plans(edited_defaults, blocks, {});
    const auto file_edit = study::make_file_plans(defaults, blocks, entries);
    const auto cleared = study::make_file_plans(defaults, blocks, {});
    for (const auto *result : {&base, &global_edit, &file_edit, &cleared}) {
        if (!*result) {
            std::cerr << result->error().message << '\n';
            return 2;
        }
    }

    std::cout << "{\"schema\":\"seedarc.seed-file-scope.r1\",\"plan_only\":true,"
                 "\"overlay_revision\":1,\"base_candidate\":{\"id\":2,\"revision\":1},"
                 "\"fixture\":\"3-files-4-blocks-r1\",\"config\":{\"defaults\":";
    write_defaults(defaults);
    std::cout << ",\"file_override\":{\"file_id\":" << entry.file_id << ",\"zstd_rank\":";
    optional_rank(entry.zstd_rank);
    std::cout << ",\"lzma2_rank\":";
    optional_rank(entry.lzma2_rank);
    std::cout << "},\"global_edit_defaults\":";
    write_defaults(edited_defaults);
    std::cout << "},\"blocks\":[";
    for (std::size_t i = 0; i < base->size(); ++i) {
        if (i != 0) {
            std::cout << ',';
        }
        const auto &identity = (*base)[i].identity;
        std::cout << "{\"file_id\":" << identity.file_id
                  << ",\"file_block_index\":" << identity.file_block_index
                  << ",\"global_block_id\":" << identity.global_block_id << ",\"base\":";
        write_plan((*base)[i].plan);
        std::cout << ",\"global_edit\":";
        write_plan((*global_edit)[i].plan);
        std::cout << ",\"file_edit\":";
        write_plan((*file_edit)[i].plan);
        std::cout << ",\"cleared\":";
        write_plan((*cleared)[i].plan);
        std::cout << '}';
    }
    std::cout << "],\"changed_global_ids\":{\"global_edit\":";
    write_changes(*base, *global_edit);
    std::cout << ",\"file_edit\":";
    write_changes(*base, *file_edit);
    std::cout << ",\"cleared\":";
    write_changes(*base, *cleared);
    std::cout << ",\"clear_from_file_edit\":";
    write_changes(*file_edit, *cleared);
    std::cout << "}}\n";
    return std::cout ? 0 : 1;
}
