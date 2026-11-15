// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Pieces shared by the DEV-017 append study tools (research containers v3 and v4): plans in the
// G5 r1 Seed layout, insertion-stable grouping, dictionary training and per-block encoding,
// plus small helpers. Research code only; nothing here is part of the SeedArc archive format.
#include "codec/codec.hpp"
#include "codec/dictionary.hpp"
#include "seedarc/archive.hpp"
#include "seedarc/archive_path.hpp"
#include "study_common.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace seedarc::research_tools::append {
namespace codec = seedarc::experimental::codec;
namespace research = seedarc::experimental::codec::research;

inline constexpr std::uint32_t chunk = 65536;
inline constexpr std::uint32_t level = 9;
inline constexpr std::size_t max_groups = 8;
inline constexpr std::uint16_t seed_revision = 1; // the G5 r1 field layout and rules below
inline constexpr std::array<std::size_t, 4> dict_sizes{0, 16384, 65536, 114688};
inline constexpr std::array<std::size_t, 2> train_prefix{chunk, 4096};
inline constexpr std::array<const char *, 4> rule_names{"dir1", "dir2", "ext", "size"};
enum : std::uint8_t { kind_store = 0, kind_zstd = 1, kind_zstd_dict = 2 };

// ---- plans (G5 r1 Seed layout) ----------------------------------------------------------------
struct Plan {
    std::uint8_t rule = 0;
    std::uint8_t k = 1;
    std::array<std::uint8_t, max_groups> d{};
    std::array<std::uint8_t, max_groups> t{};
    auto operator<=>(const Plan &) const = default;
};
inline Plan canonical(Plan p) {
    if (p.k == 1) {
        p.rule = 0;
    }
    for (std::size_t i = 0; i < max_groups; ++i) {
        if (i >= p.k) {
            p.d[i] = 0;
            p.t[i] = 0;
        } else if (p.d[i] == 0) {
            p.t[i] = 0;
        }
    }
    return p;
}
inline std::uint32_t encode_seed(const Plan &p) {
    std::uint32_t s = p.rule | (static_cast<std::uint32_t>(p.k - 1) << 2);
    for (std::size_t i = 0; i < max_groups; ++i) {
        s |= static_cast<std::uint32_t>(p.d[i] | (p.t[i] << 2)) << (5 + 3 * i);
    }
    return s;
}
inline std::optional<Plan> decode_seed(std::uint32_t s) {
    if (s >> 29) {
        return std::nullopt;
    }
    Plan p;
    p.rule = static_cast<std::uint8_t>(s & 3);
    p.k = static_cast<std::uint8_t>(((s >> 2) & 7) + 1);
    for (std::size_t i = 0; i < max_groups; ++i) {
        const auto f = (s >> (5 + 3 * i)) & 7;
        p.d[i] = static_cast<std::uint8_t>(f & 3);
        p.t[i] = static_cast<std::uint8_t>(f >> 2);
    }
    return p;
}

inline std::string group_key(int rule, std::string_view path, std::uint64_t size) {
    switch (rule) {
    case 0: {
        const auto slash = path.find('/');
        return slash == std::string_view::npos ? std::string{} : std::string{path.substr(0, slash)};
    }
    case 1: {
        const auto first = path.find('/');
        if (first == std::string_view::npos) {
            return {};
        }
        const auto second = path.find('/', first + 1);
        return std::string{second == std::string_view::npos ? path.substr(0, first)
                                                            : path.substr(0, second)};
    }
    case 2: {
        const auto base = path.substr(path.rfind('/') + 1); // npos + 1 == 0
        const auto dot = base.rfind('.');
        std::string ext = dot == std::string_view::npos || dot == 0
                              ? std::string{}
                              : std::string{base.substr(dot + 1)};
        for (auto &c : ext) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
        }
        return ext;
    }
    default:
        return size <= 1024 ? "s" : size <= 16384 ? "m" : "l";
    }
}

// Insertion-stable grouping: the key table names up to k-1 groups; everything else is "other".
struct Grouping {
    std::map<std::string, std::uint8_t> keys; // the key table
    std::uint8_t other = 0;                   // the "other" group
    std::size_t groups() const { return static_cast<std::size_t>(other) + 1; }
    std::uint8_t of(int rule, std::string_view path, std::uint64_t size) const {
        const auto it = keys.find(group_key(rule, path, size));
        return it == keys.end() ? other : it->second;
    }
    bool operator==(const Grouping &) const = default;
};
inline Grouping initial_grouping(const Plan &p, const std::vector<InputFile> &files) {
    std::map<std::string, std::uint64_t> bytes;
    for (const auto &f : files) {
        bytes[group_key(p.rule, f.path, f.data.size())] += f.data.size();
    }
    std::vector<std::pair<std::uint64_t, std::string>> order;
    for (const auto &[key, n] : bytes) {
        order.emplace_back(n, key);
    }
    std::ranges::sort(order, [](const auto &a, const auto &b) {
        return a.first != b.first ? a.first > b.first : a.second < b.second;
    });
    Grouping g;
    const auto named = std::min<std::size_t>(p.k - 1, order.size());
    for (std::size_t r = 0; r < named; ++r) {
        g.keys[order[r].second] = static_cast<std::uint8_t>(r);
    }
    g.other = static_cast<std::uint8_t>(named);
    return g;
}

// ---- encoding -----------------------------------------------------------------------------------
struct Block {
    std::uint64_t offset;
    std::uint32_t stored;
    std::uint8_t kind;
    std::uint32_t crc;
    bool operator==(const Block &) const = default;
};

inline std::expected<std::vector<std::byte>, Error>
train(const std::vector<const InputFile *> &members, std::uint8_t d, std::uint8_t t) {
    std::vector<const InputFile *> order;
    for (const auto *f : members) {
        if (!f->data.empty()) {
            order.push_back(f);
        }
    }
    std::ranges::sort(order, {}, [](const InputFile *f) { return fnv1a(f->path); });
    std::vector<std::byte> samples;
    std::vector<std::size_t> sizes;
    for (const auto *f : order) {
        const auto n = std::min<std::size_t>(f->data.size(), train_prefix[t]);
        if (samples.size() + n > research::max_training_bytes) {
            break;
        }
        samples.insert(samples.end(), f->data.begin(),
                       f->data.begin() + static_cast<std::ptrdiff_t>(n));
        sizes.push_back(n);
    }
    auto trained = research::train_dictionary(samples, sizes, dict_sizes[d]);
    if (!trained) {
        return std::vector<std::byte>{}; // falls back to no dictionary
    }
    return std::move(*trained);
}

// Encodes the blocks of `f` (group dictionary `dict`, may be empty) appended to `out`; each
// block's offset is its position in `out`.
inline std::expected<std::vector<Block>, Error>
encode_blocks(const InputFile &f, std::span<const std::byte> dict, std::vector<std::byte> &out) {
    auto plain = codec::Encoder::create(codec::zstd_id, level);
    if (!plain) {
        return std::unexpected(plain.error());
    }
    std::optional<research::DictionaryEncoder> encoder;
    if (!dict.empty()) {
        auto created = research::DictionaryEncoder::create(level, dict);
        if (!created) {
            return std::unexpected(created.error());
        }
        encoder.emplace(std::move(*created));
    }
    std::vector<Block> blocks;
    std::vector<std::byte> packed(chunk);
    for (std::size_t at = 0; at < f.data.size(); at += chunk) {
        const auto raw =
            std::span{f.data}.subspan(at, std::min<std::size_t>(chunk, f.data.size() - at));
        const auto room = std::span{packed}.first(raw.size() - 1);
        auto size = encoder ? encoder->encode(raw, room) : plain->encode(raw, room);
        if (!size) {
            return std::unexpected(size.error());
        }
        const auto kind = *size != 0 ? (encoder ? kind_zstd_dict : kind_zstd) : kind_store;
        const auto stored = *size != 0 ? std::span<const std::byte>{packed}.first(*size) : raw;
        blocks.push_back({out.size(), static_cast<std::uint32_t>(stored.size()), kind,
                          seedarc::experimental::crc32c(raw)});
        out.insert(out.end(), stored.begin(), stored.end());
    }
    return blocks;
}

// ---- helpers ------------------------------------------------------------------------------------
inline std::expected<std::uint32_t, Error> hex32(std::string_view text) {
    if (text.starts_with("0x")) {
        text.remove_prefix(2);
    }
    if (text.empty() || text.size() > 8) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "bad plan"});
    }
    std::uint32_t v = 0;
    for (const auto c : text) {
        const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (d < 0) {
            return std::unexpected(Error{ErrorCode::invalid_argument, "bad plan"});
        }
        v = (v << 4) | static_cast<std::uint32_t>(d);
    }
    return v;
}

inline std::expected<void, Error> write_new(const fs::path &path, std::span<const std::byte> bytes) {
    if (fs::exists(path)) {
        return std::unexpected(Error{ErrorCode::output_conflict, "output exists"});
    }
    std::ofstream stream(path, std::ios::binary);
    if (!stream.write(reinterpret_cast<const char *>(bytes.data()),
                      static_cast<std::streamsize>(bytes.size())) ||
        !stream.flush()) {
        return std::unexpected(Error{ErrorCode::io_error, "write failed"});
    }
    return {};
}

// The production manifest checks (ADR-0005) over a list of file paths, with the implied parent
// directories added: per-path profile, duplicates, case collisions, a file used as a directory.
inline std::expected<void, Error> check_manifest(const std::vector<std::string> &paths) {
    std::vector<seedarc::experimental::PathEntry> entries;
    std::set<std::string> directories;
    for (const auto &p : paths) {
        entries.push_back({p, false});
        for (auto slash = p.find('/'); slash != std::string::npos; slash = p.find('/', slash + 1)) {
            directories.insert(p.substr(0, slash));
        }
    }
    for (const auto &d : directories) {
        entries.push_back({d, true});
    }
    seedarc::experimental::PathLimits limits;
    limits.max_entries = entries.size();
    limits.max_metadata_bytes = 64 * 1024 * 1024;
    auto result = seedarc::experimental::validate_manifest_paths(entries, limits);
    if (!result) {
        return std::unexpected(result.error());
    }
    return {};
}
} // namespace seedarc::research_tools::append
