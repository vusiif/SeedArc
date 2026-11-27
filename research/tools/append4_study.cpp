// SPDX-License-Identifier: GPL-3.0-or-later
// DEV-017 research tool: research container v4 (research/ideas/seed-append-r1.md, 7.5).
// EXPERIMENTAL; not the SeedArc archive format; nothing frozen. v3 (append_study.cpp) stays as
// the tail-only baseline.
//
//   header (32) | gen 0: GEN record, its new dictionaries, data and redundancy records |
//   [superseded trailer + footer] | gen 1: ... | trailer | footer (24)
//
// Every generation starts with a GEN record (its Seed, key table and group table) followed by
// the dictionaries it introduces, so any prefix of the file carries what its data needs. A
// generation may keep the previous generation's Seed and dictionaries or bring its own (the
// generations form a queue); a Seed only ever applies to its own generation's files.
//
// Redundancy level (archive-wide, in the header; chosen at pack time, by recommendation or by
// the user):
//   off         no per-file records; files are listed only by trailers
//   checkpoint  a CKP record after about every N data bytes lists the files since the previous
//               one (zstd-compressed listing), so the cost hardly depends on the file count
//   file        an FIL record in front of every file
// Solid mode writes an FRM record in front of every frame unless the level is off.
//
// Records: tag (8) | body length (4) | body | CRC32C(body) (4). Every body starts with the
// record's own offset, so a record is only valid where it claims to be (copies inside stored
// data or old trailers are ignored). The trailer copies every GEN body and lists every file
// with explicit offsets; reading the whole container cross-checks the trailer against the
// records and fails closed on any difference. Recovery from a truncated or damaged file uses
// the newest intact trailer in the prefix plus every valid record, verifies every file by CRC,
// refuses conflicting copies and applies the production manifest checks (ADR-0005).
#include "append_common.hpp"
#include "seedarc/safe_output.hpp"
#include "solid_plan.hpp"
#include "splitmix64/splitmix64.hpp"

#ifdef _WIN32
#include <shellapi.h>
#endif
#include <array>
#include <atomic>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <iostream>
#include <ranges>
#include <thread>
#include <tuple>

namespace {
using namespace seedarc::research_tools;
using namespace seedarc::research_tools::append;

constexpr std::string_view usage =
    "Research-only append study (container v4); not the SeedArc archive format.\n"
    "  append4_study --jobs N COMMAND ...   (1-16 solid frame encoder workers; same bytes)\n"
    "  append4_study pack seed|explicit|solid PLAN_HEX INPUT_DIR OUT [auto|off|checkpoint|file]"
    " [CHECKPOINT_BYTES]\n"
    "  append4_study append OLD NEW_DIR OUT [segment|regroup] [--plan PLAN_HEX]\n"
    "  append4_study verify CONTAINER INPUT_DIR      (every file, byte for byte)\n"
    "  append4_study peek CONTAINER INPUT_DIR N      (N cold single-file extractions)\n"
    "  append4_study partial CONTAINER INPUT_DIR     (recovery from truncated prefixes)\n"
    "  append4_study split CONTAINER VOLUME_BYTES NEW_DIR [none|gen|dicts]   (carry; default gen)\n"
    "  append4_study vappend VOLUME_DIR NEW_DIR [--plan PLAN_HEX]   (writes new volumes only)\n"
    "  append4_study vverify VOLUME_DIR INPUT_DIR\n"
    "  append4_study vpartial VOLUME_DIR INPUT_DIR   (recovery with each volume missing)\n"
    "  append4_study search seed|coord START INPUT_DIR BUDGET_BYTES PRNG_SEED [restart|local]"
    " [STALL]\n"
    "      START: default (G5 start plan) or PLAN_HEX; restart = cold (G5), local = stop when"
    " stuck; STALL default 20\n"
    "  append4_study selftest INPUT_DIR\n"
    "  append4_study pack seed-solid|explicit-solid SOLID_PLAN_HEX INPUT_DIR OUT [off]\n"
    "  append4_study payload-compare LEFT RIGHT\n"
    "  append4_study solid-selftest INPUT_DIR\n"
    "  append4_study solid-search seed|explicit START INPUT_DIR OUT BUDGET_BYTES PRNG_SEED "
    "[near|enumerate]\n"
    "      START: default, Seed hex (seed), or rule:k (explicit)\n"
    "  append4_study bounded-pack seed|explicit FRAME_LIMIT_BYTES INPUT_DIR OUT\n"
    "  append4_study bounded-append FRAME_LIMIT_BYTES OLD NEW_DIR OUT\n"
    "  append4_study access-profile ARCHIVE INPUT_DIR UNIFORM_SAMPLE_COUNT\n"
    "  append4_study payload-fault-profile ARCHIVE INPUT_DIR\n"
    "  append4_study extract ARCHIVE PATH OUT_DIR BASENAME   (indexed read; revision-5 solid)\n"
    "  append4_study extract-profile ARCHIVE INPUT_DIR UNIFORM_SAMPLE_COUNT EMPTY_OUT_DIR\n"
    "  append4_study recipe-pack seed|explicit RECIPE INPUT_DIR OUT   (ADR-0006, mapping rev 3)\n"
    "  append4_study recipe-append OLD NEW_DIR OUT [RECIPE]   (no RECIPE: keep the last one)\n"
    "  append4_study describe ARCHIVE\n"
    "  append4_study consolidate-check ARCHIVE [RECIPE]   (measure a one-generation rebuild)\n"
    "  append4_study consolidate ARCHIVE OUT [RECIPE]   (write it; default: last recipe)\n"
    "  append4_study drift-probe OLD_DIR NEW_DIR ZSTD_LEVEL   (append-drift references; no format)\n"
    "  append4_study codec-probe[-classes] DIR VARIANTS   (e.g. zstd-19-ldm,lzma2-9e-bcj; no format)\n"
    "  append4_study codec-probe-framed DIR VARIANTS <16MiB|32MiB|64MiB|128MiB|256MiB>\n"
    "  append4_study dup-probe DIR VARIANT <whole|16MiB|...|256MiB>   (whole-file duplicates; no format)\n"
    "  append4_study cut-probe DIR VARIANT <16MiB|...|256MiB>   (frame cut rules; no format)\n"
    "      RECIPE: solid/zstd-9/g<k>-r<rule>/frame-<whole|16MiB|32MiB|64MiB|128MiB|256MiB>"
    " or 0x Seed\n";
using Tag = std::array<char, 8>;
constexpr Tag magic{'S', 'A', 'D', 'X', 'R', '0', '0', '4'};
constexpr Tag footer_magic{'S', 'A', 'D', 'X', 'E', 'N', 'D', '4'};
constexpr std::array<Tag, 4> record_tags{{{'S', 'A', 'D', 'X', 'G', 'E', 'N', '4'},
                                          {'S', 'A', 'D', 'X', 'F', 'I', 'L', '4'},
                                          {'S', 'A', 'D', 'X', 'C', 'K', 'P', '4'},
                                          {'S', 'A', 'D', 'X', 'F', 'R', 'M', '4'}}};
enum RecordType : std::uint8_t { rec_gen = 0, rec_file = 1, rec_checkpoint = 2, rec_frame = 3 };
constexpr std::uint32_t version = 4;
constexpr std::size_t header_bytes = 32;
constexpr std::size_t footer_bytes = 24;
constexpr std::size_t record_overhead = 16;
constexpr std::size_t max_record_body = std::size_t{1} << 24;
// Largest raw solid frame a reader accepts. Only the fuzz build lowers it (append4_fuzz.cpp), so
// a mutated size field cannot make every run allocate a gigabyte.
#ifndef SEEDARC_APPEND4_MAX_FRAME_RAW
#define SEEDARC_APPEND4_MAX_FRAME_RAW research::max_solid_bytes
#endif
constexpr std::uint64_t max_frame_raw = SEEDARC_APPEND4_MAX_FRAME_RAW;
constexpr std::uint32_t max_generations = 1024;
enum : std::uint32_t { flag_seed = 1, flag_solid = 2 };
enum : std::uint32_t { red_off = 0, red_checkpoint = 1, red_file = 2 };
constexpr std::array<const char *, 3> redundancy_names{"off", "checkpoint", "file"};
constexpr std::uint32_t min_checkpoint_bytes = 4096;
constexpr std::uint32_t max_checkpoint_bytes = 1U << 30;
// Recommendation rule "rec-r0". PROVISIONAL placeholders, not measured: the real thresholds are
// set on the dev split from H-partial and then frozen (seed-append-r1 7.5.2).
constexpr std::uint32_t default_checkpoint_bytes = 1U << 20;
constexpr std::uint64_t rec_small_archive = 4ULL << 20;
constexpr double rec_overhead_budget = 0.01;

Error bad(const char *what) {
    return {ErrorCode::integrity_error, std::string{"research container v4: "} + what};
}
int fail(const Error &e) {
    std::cerr << e.message << '\n';
    return 1;
}
std::span<const std::byte> as_bytes(const Tag &t) { return std::as_bytes(std::span{t}); }
void put_text(std::vector<std::byte> &out, std::string_view s) {
    put(out, s.size(), 2);
    for (const auto c : s) {
        out.push_back(static_cast<std::byte>(c));
    }
}
std::optional<std::string> get_text(Reader &r, std::size_t min_len) {
    const auto len = r.get(2);
    if (!len || *len < min_len || *len > max_path || r.bytes.size() - r.at < *len) {
        return std::nullopt;
    }
    std::string s(reinterpret_cast<const char *>(r.bytes.data() + r.at),
                  static_cast<std::size_t>(*len));
    r.at += static_cast<std::size_t>(*len);
    return s;
}

// ---- header -------------------------------------------------------------------------------------
struct Header {
    std::uint32_t flags = 0;
    std::uint32_t redundancy = red_off;
    std::uint32_t checkpoint_bytes = 0;
    std::uint32_t revision = version;
    bool seed() const { return (flags & flag_seed) != 0; }
    bool solid() const { return (flags & flag_solid) != 0; }
    bool explicit_groups() const { return !seed() && !solid(); }
};
std::vector<std::byte> header_of(const Header &h) {
    std::vector<std::byte> out(magic.size());
    std::memcpy(out.data(), magic.data(), magic.size());
    put(out, h.revision, 4);
    put(out, chunk, 4);
    put(out, level, 4);
    put(out, h.flags, 4);
    put(out, h.redundancy, 4);
    put(out, h.checkpoint_bytes, 4);
    return out; // exactly header_bytes
}
std::expected<Header, Error> parse_header(std::span<const std::byte> bytes) {
    if (bytes.size() < header_bytes || std::memcmp(bytes.data(), magic.data(), magic.size()) != 0) {
        return std::unexpected(bad("bad magic"));
    }
    Reader r{bytes, magic.size()};
    const auto ver = r.get(4), ck = r.get(4), lv = r.get(4), flags = r.get(4), red = r.get(4),
               cpb = r.get(4);
    const bool strong = ver && *ver == 5;
    if (!cpb || (*ver != version && !strong) || *ck != chunk || *lv != level ||
        (*flags & ~3ULL) != 0 || (!strong && *flags == (flag_seed | flag_solid)) ||
        (strong && ((*flags & flag_solid) == 0 || *red != red_off)) || *red > red_file ||
        (*red == red_checkpoint) != (*cpb != 0) ||
        (*cpb != 0 && (*cpb < min_checkpoint_bytes || *cpb > max_checkpoint_bytes))) {
        return std::unexpected(bad("bad header"));
    }
    return Header{static_cast<std::uint32_t>(*flags), static_cast<std::uint32_t>(*red),
                  static_cast<std::uint32_t>(*cpb), static_cast<std::uint32_t>(*ver)};
}

// ---- the container model ------------------------------------------------------------------------
struct GenGroup {
    std::uint64_t dict_offset = 0;
    std::uint32_t dict_size = 0;
    std::uint32_t dict_crc = 0;
    bool operator==(const GenGroup &) const = default;
};
struct Gen {
    std::uint32_t index = 0;
    std::uint32_t first_file = 0;
    std::uint32_t files_after = 0; // trailer only
    std::uint64_t record_offset = 0;
    bool has_seed = false;
    std::uint16_t mapping_revision = seed_revision;
    Plan plan;
    std::optional<RecipeFields> recipe; // solid-recipe-r1 (mapping revision 3) only
    Grouping grouping;
    std::vector<GenGroup> groups;
    std::vector<std::byte> body; // the GEN record body as written
};
struct Entry {
    std::string path;
    std::uint64_t size = 0;
    std::uint32_t gen = 0;
    std::uint8_t group = 0;
    std::vector<Block> blocks;      // block mode
    std::uint32_t frame = 0;        // solid mode
    std::uint64_t frame_offset = 0; // solid mode
    std::uint32_t file_crc = 0;     // solid mode
    bool operator==(const Entry &) const = default;
};
struct Frame {
    std::uint64_t offset = 0, stored = 0, raw = 0;
    std::uint32_t gen = 0;
    std::uint8_t group = 0;
    bool operator==(const Frame &) const = default;
};
struct Model {
    Header header;
    std::vector<Gen> gens;
    std::vector<std::uint64_t> data_start; // per generation: after its record and new dictionaries
    std::vector<Entry> entries;
    std::vector<Frame> frames;
    std::uint64_t trailer_offset = 0;
    std::array<std::uint64_t, 4> record_bytes{}; // per record type (from the cross-check scan)
};

std::uint8_t group_of(const Gen &g, std::string_view path, std::uint64_t size) {
    return g.grouping.of(g.plan.rule, path, size);
}

// ---- records ------------------------------------------------------------------------------------
void put_record(std::vector<std::byte> &out, RecordType type, std::span<const std::byte> body) {
    const auto tag = as_bytes(record_tags[type]);
    out.insert(out.end(), tag.begin(), tag.end());
    put(out, body.size(), 4);
    out.insert(out.end(), body.begin(), body.end());
    put(out, seedarc::experimental::crc32c(body), 4);
}
struct Record {
    RecordType type;
    std::span<const std::byte> body;
    std::size_t end;
};
// `base` is the absolute container offset of bytes[0] (non-zero when only a window was read).
std::optional<Record> read_record(std::span<const std::byte> bytes, std::size_t at,
                                  std::uint64_t base = 0) {
    if (at > bytes.size() || bytes.size() - at < record_overhead) {
        return std::nullopt;
    }
    std::optional<RecordType> type;
    for (std::size_t i = 0; i < record_tags.size(); ++i) {
        if (std::memcmp(bytes.data() + at, record_tags[i].data(), 8) == 0) {
            type = static_cast<RecordType>(i);
        }
    }
    Reader r{bytes, at + 8};
    const auto len = r.get(4);
    if (!type || !len || *len < 8 || *len > max_record_body ||
        bytes.size() - at - record_overhead < *len) {
        return std::nullopt;
    }
    const auto body = bytes.subspan(at + 12, static_cast<std::size_t>(*len));
    Reader c{bytes, at + 12 + static_cast<std::size_t>(*len)};
    Reader self{body, 0};
    if (c.get(4) != seedarc::experimental::crc32c(body) || self.get(8) != base + at) {
        return std::nullopt;
    }
    return Record{*type, body, at + record_overhead + static_cast<std::size_t>(*len)};
}
// The first valid record at or after `from` (its position goes to `where`).
std::optional<Record> find_record(std::span<const std::byte> bytes, std::size_t from,
                                  std::size_t &where) {
    const auto sadx = as_bytes(magic).first(4);
    for (std::size_t pos = from; pos < bytes.size();) {
        const auto it = std::search(bytes.begin() + static_cast<std::ptrdiff_t>(pos), bytes.end(),
                                    sadx.begin(), sadx.end());
        if (it == bytes.end()) {
            break;
        }
        pos = static_cast<std::size_t>(it - bytes.begin());
        if (auto r = read_record(bytes, pos)) {
            where = pos;
            return r;
        }
        ++pos;
    }
    return std::nullopt;
}

std::vector<std::byte> gen_body(const Gen &g) {
    std::vector<std::byte> b;
    put(b, g.record_offset, 8);
    put(b, g.index, 4);
    put(b, g.first_file, 4);
    if (g.mapping_revision == solid_recipe_revision && g.recipe) {
        const SolidRecipe r{g.plan, *g.recipe};
        put(b, g.has_seed ? solid_recipe_revision : 0, 2);
        if (g.has_seed) {
            put(b, 4, 2);
            put(b, encode_recipe_seed(r), 4);
        } else {
            const auto words = encode_recipe_explicit(r);
            put(b, 8, 2);
            put(b, words[0], 4);
            put(b, words[1], 4);
        }
    } else if (g.mapping_revision == solid_seed_revision) {
        put(b, g.has_seed ? solid_seed_revision : 0, 2);
        put(b, 4, 2);
        put(b, g.has_seed ? encode_solid_seed(g.plan) : encode_solid_explicit(g.plan), 4);
    } else if (g.has_seed) {
        put(b, seed_revision, 2);
        put(b, 4, 2);
        put(b, encode_seed(g.plan), 4);
    } else {
        put(b, 0, 2);
        put(b, 0, 2);
    }
    put(b, g.grouping.keys.size(), 4);
    for (const auto &[key, id] : g.grouping.keys) {
        put_text(b, key);
        b.push_back(static_cast<std::byte>(id));
    }
    put(b, g.groups.size(), 4);
    for (const auto &grp : g.groups) {
        put(b, grp.dict_offset, 8);
        put(b, grp.dict_size, 4);
        put(b, grp.dict_crc, 4);
    }
    return b;
}
std::expected<Gen, Error> parse_gen_body(std::span<const std::byte> body, const Header &h) {
    Reader r{body, 0};
    Gen g;
    const auto self = r.get(8), index = r.get(4), first = r.get(4), rev = r.get(2), len = r.get(2);
    if (!len || *index >= max_generations || *first > max_files) {
        return std::unexpected(bad("bad generation record"));
    }
    g.record_offset = *self;
    g.index = static_cast<std::uint32_t>(*index);
    g.first_file = static_cast<std::uint32_t>(*first);
    const bool recipe_seed = h.revision == 5 && h.seed() && *rev == solid_recipe_revision;
    const bool recipe_explicit = h.revision == 5 && !h.seed() && *rev == 0 && *len == 8;
    if (recipe_seed || recipe_explicit) {
        std::optional<SolidRecipe> recipe;
        if (recipe_seed) {
            const auto value = *len == 4 ? r.get(4) : std::nullopt;
            recipe = value ? decode_recipe_seed(static_cast<std::uint32_t>(*value)) : std::nullopt;
        } else {
            const auto word0 = r.get(4), word1 = r.get(4);
            recipe = word1 ? decode_recipe_explicit(static_cast<std::uint32_t>(*word0),
                                                    static_cast<std::uint32_t>(*word1))
                           : std::nullopt;
        }
        if (!recipe) {
            return std::unexpected(bad("bad solid recipe record"));
        }
        g.has_seed = h.seed();
        g.mapping_revision = solid_recipe_revision;
        g.plan = recipe->plan;
        g.recipe = recipe->fields;
    } else if (h.revision == 5) {
        const auto value = r.get(4);
        const auto plan =
            value ? (h.seed() ? decode_solid_seed(static_cast<std::uint32_t>(*value))
                              : decode_solid_explicit(static_cast<std::uint32_t>(*value)))
                  : std::nullopt;
        if (*rev != (h.seed() ? solid_seed_revision : 0) || *len != 4 || !plan) {
            return std::unexpected(bad("bad solid strategy record"));
        }
        g.has_seed = h.seed();
        g.mapping_revision = solid_seed_revision;
        g.plan = *plan;
    } else if (h.seed()) {
        const auto seed = r.get(4);
        const auto plan = seed ? decode_seed(static_cast<std::uint32_t>(*seed)) : std::nullopt;
        if (*rev != seed_revision || *len != 4 || !plan || canonical(*plan) != *plan) {
            return std::unexpected(bad("bad seed record"));
        }
        g.has_seed = true;
        g.plan = *plan;
    } else if (*rev != 0 || *len != 0) {
        return std::unexpected(bad("unexpected seed record"));
    }
    const auto nkeys = r.get(4);
    if (!nkeys || *nkeys >= max_groups) {
        return std::unexpected(bad("bad key table"));
    }
    std::set<std::uint8_t> ids;
    for (std::uint64_t i = 0; i < *nkeys; ++i) {
        auto key = get_text(r, 0);
        const auto id = r.get(1);
        if (!key || !id || !ids.insert(static_cast<std::uint8_t>(*id)).second ||
            !g.grouping.keys.emplace(std::move(*key), static_cast<std::uint8_t>(*id)).second) {
            return std::unexpected(bad("bad key"));
        }
    }
    const auto ngroups = r.get(4);
    // Distinct ids with the largest equal to nkeys - 1 cover the range [0, nkeys - 1].
    if (!ngroups || *ngroups == 0 || *ngroups > max_groups || *nkeys != *ngroups - 1 ||
        (!ids.empty() && *ids.rbegin() != *nkeys - 1) ||
        ((g.has_seed || h.revision == 5) && *ngroups > g.plan.k)) {
        return std::unexpected(bad("key table does not name every group but the last"));
    }
    g.grouping.other = static_cast<std::uint8_t>(*ngroups - 1);
    for (std::uint64_t i = 0; i < *ngroups; ++i) {
        const auto off = r.get(8), size = r.get(4), crc = r.get(4);
        if (!crc || *size > research::max_dictionary_bytes || (h.revision == 5 && *size != 0) ||
            (*size == 0 && (*off != 0 || *crc != 0))) {
            return std::unexpected(bad("bad group table"));
        }
        g.groups.push_back(
            {*off, static_cast<std::uint32_t>(*size), static_cast<std::uint32_t>(*crc)});
    }
    if (r.at != body.size()) {
        return std::unexpected(bad("trailing bytes in a generation record"));
    }
    g.body.assign(body.begin(), body.end());
    return g;
}

// Checks where a generation's dictionaries are: new ones directly after its record, in group
// order; inherited ones before the record (and, when `earlier` is given, equal to a group of an
// earlier generation). Every dictionary must be present (below `limit`) with a matching CRC.
// Returns where the generation's data starts.
std::expected<std::uint64_t, Error> check_gen_dicts(std::span<const std::byte> bytes, const Gen &g,
                                                    std::uint64_t record_end,
                                                    const std::vector<Gen> *earlier) {
    auto cursor = record_end;
    for (const auto &grp : g.groups) {
        if (grp.dict_size == 0) {
            continue;
        }
        if (grp.dict_offset >= record_end) {
            if (grp.dict_offset != cursor) {
                return std::unexpected(bad("new dictionaries are not laid out after the record"));
            }
            cursor += grp.dict_size;
        } else {
            bool known = earlier == nullptr;
            if (earlier != nullptr) {
                for (const auto &e : *earlier) {
                    known = known || std::ranges::find(e.groups, grp) != e.groups.end();
                }
            }
            if (grp.dict_offset < header_bytes || grp.dict_offset > g.record_offset ||
                g.record_offset - grp.dict_offset < grp.dict_size || !known) {
                return std::unexpected(bad("bad inherited dictionary"));
            }
        }
        if (grp.dict_offset > bytes.size() || bytes.size() - grp.dict_offset < grp.dict_size) {
            return std::unexpected(bad("dictionary outside the container"));
        }
        const auto dict = bytes.subspan(static_cast<std::size_t>(grp.dict_offset), grp.dict_size);
        if (seedarc::experimental::crc32c(dict) != grp.dict_crc) {
            return std::unexpected(bad("dictionary checksum mismatch"));
        }
    }
    return cursor;
}

// Per-file description shared by FIL records and CKP listings (offsets are implicit).
void put_file_info(std::vector<std::byte> &b, const Entry &e, bool explicit_groups) {
    put_text(b, e.path);
    put(b, e.size, 8);
    if (explicit_groups) {
        b.push_back(static_cast<std::byte>(e.group));
    }
    put(b, e.blocks.size(), 4);
    for (const auto &blk : e.blocks) {
        put(b, blk.stored, 4);
        b.push_back(static_cast<std::byte>(blk.kind));
        put(b, blk.crc, 4);
    }
}
// Reads one file description; the block offsets are laid out from `data_at`.
std::expected<Entry, Error> read_file_info(Reader &r, const Gen &g, const Header &h,
                                           std::uint64_t data_at) {
    Entry e;
    auto path = get_text(r, 1);
    const auto size = path ? r.get(8) : std::nullopt;
    if (!size || *size > max_input) {
        return std::unexpected(bad("bad file description"));
    }
    e.path = std::move(*path);
    e.size = *size;
    e.gen = g.index;
    if (h.explicit_groups()) {
        const auto grp = r.get(1);
        if (!grp || *grp >= g.groups.size()) {
            return std::unexpected(bad("bad file group"));
        }
        e.group = static_cast<std::uint8_t>(*grp);
    } else {
        e.group = group_of(g, e.path, e.size);
    }
    const bool has_dict = g.groups[e.group].dict_size != 0;
    const auto nblocks = r.get(4);
    if (!nblocks || *nblocks != (*size + chunk - 1) / chunk) {
        return std::unexpected(bad("bad block count"));
    }
    for (std::uint64_t b = 0; b < *nblocks; ++b) {
        const auto stored = r.get(4), kind = r.get(1), crc = r.get(4);
        const auto raw = std::min<std::uint64_t>(chunk, *size - b * chunk);
        if (!crc || *kind > kind_zstd_dict || *stored == 0 || *stored > raw ||
            (*kind == kind_store) != (*stored == raw) || (*kind == kind_zstd_dict && !has_dict) ||
            (*kind == kind_zstd && has_dict)) {
            return std::unexpected(bad("bad block"));
        }
        e.blocks.push_back({data_at, static_cast<std::uint32_t>(*stored),
                            static_cast<std::uint8_t>(*kind), static_cast<std::uint32_t>(*crc)});
        data_at += *stored;
    }
    return e;
}

// ---- scanning the records (front to back, no trailer needed) ------------------------------------
// Which logical bytes are really there (a volume set with missing volumes has zero-filled holes).
// Without one, every byte is present.
struct Presence {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> runs; // sorted, merged [begin, end)
    void add(std::uint64_t begin, std::uint64_t end) {
        runs.emplace_back(begin, end);
        std::ranges::sort(runs);
        std::vector<std::pair<std::uint64_t, std::uint64_t>> merged;
        for (const auto &r : runs) {
            if (!merged.empty() && r.first <= merged.back().second) {
                merged.back().second = std::max(merged.back().second, r.second);
            } else {
                merged.push_back(r);
            }
        }
        runs = std::move(merged);
    }
    // How many bytes from `at` are present without a hole.
    std::uint64_t run(std::uint64_t at) const {
        for (const auto &[b, e] : runs) {
            if (at >= b && at < e) {
                return e - at;
            }
        }
        return 0;
    }
    bool covers(std::uint64_t at, std::uint64_t len) const { return len == 0 || run(at) >= len; }
};
bool covered(const Presence *p, std::uint64_t at, std::uint64_t len) {
    return p == nullptr || p->covers(at, len);
}
bool blocks_present(const Presence *p, const std::vector<Block> &blocks) {
    return std::ranges::all_of(blocks,
                               [&](const Block &b) { return covered(p, b.offset, b.stored); });
}

struct Scan {
    std::map<std::uint32_t, Gen> gens;
    std::map<std::uint32_t, std::uint64_t> data_start;
    std::map<std::uint32_t, Entry> entries; // by file index
    std::vector<Frame> frames;
    std::vector<std::vector<std::byte>> frame_raw;
    std::vector<std::size_t> frame_produced; // leading bytes decoded (decode mode)
    std::vector<std::pair<std::uint32_t, std::uint32_t>> checkpoints; // (first file, count)
    std::array<std::uint64_t, 4> record_bytes{};
};

std::expected<void, Error> add_entry(Scan &s, std::uint64_t index, Entry e) {
    if (index >= max_files) {
        return std::unexpected(bad("file index out of range"));
    }
    const auto [it, inserted] = s.entries.emplace(static_cast<std::uint32_t>(index), e);
    if (!inserted && it->second != e) {
        return std::unexpected(bad("conflicting records for one file"));
    }
    return {};
}

// Walks `bytes` from the header, accepting every valid record and skipping anything else (data
// in off / checkpoint mode, superseded trailers, damage). With `decode`, solid frames are
// decoded as far as they are present.
std::expected<Scan, Error> scan(std::span<const std::byte> bytes, const Header &h, bool decode,
                                const Presence *presence = nullptr) {
    Scan s;
    std::size_t p = header_bytes;
    std::uint64_t region_start = p; // where the data covered by the next CKP may start
    while (true) {
        std::size_t q = 0;
        const auto r = find_record(bytes, p, q);
        if (!r) {
            break;
        }
        Reader b{r->body, 8};
        p = q + 1; // where to look next if this record turns out unusable
        if (r->type == rec_gen) {
            auto g = parse_gen_body(r->body, h);
            if (!g || s.gens.contains(g->index)) {
                continue;
            }
            const auto ds = check_gen_dicts(bytes, *g, r->end, nullptr);
            s.record_bytes[rec_gen] += r->end - q;
            if (!ds) {
                p = r->end; // its dictionaries are cut or damaged: its data cannot be decoded
                region_start = p;
                continue;
            }
            s.data_start[g->index] = *ds;
            s.gens.emplace(g->index, std::move(*g));
            p = static_cast<std::size_t>(*ds);
            region_start = p;
            continue;
        }
        const auto gen = b.get(4);
        if (!gen || !s.gens.contains(static_cast<std::uint32_t>(*gen))) {
            continue;
        }
        const auto &g = s.gens.at(static_cast<std::uint32_t>(*gen));
        if (r->type == rec_file && !h.solid() && h.redundancy == red_file) {
            const auto index = b.get(4);
            if (!index) {
                continue;
            }
            auto e = read_file_info(b, g, h, r->end);
            if (!e || b.at != r->body.size()) {
                continue;
            }
            std::uint64_t stored = 0;
            for (const auto &blk : e->blocks) {
                stored += blk.stored;
            }
            s.record_bytes[rec_file] += r->end - q;
            if (r->end + stored > bytes.size()) {
                break; // the file's data is cut off, and so is everything after it
            }
            if (auto a = add_entry(s, *index, std::move(*e)); !a) {
                return std::unexpected(a.error());
            }
            p = static_cast<std::size_t>(r->end + stored);
            region_start = p;
        } else if (r->type == rec_checkpoint && !h.solid() && h.redundancy == red_checkpoint) {
            const auto first = b.get(4), nfiles = b.get(4), covered = b.get(8), raw_len = b.get(4);
            if (!raw_len || *nfiles == 0 || *nfiles > max_files || *covered < region_start ||
                *covered > q || *raw_len == 0 || *raw_len > max_record_body) {
                continue;
            }
            std::vector<std::byte> listing(static_cast<std::size_t>(*raw_len));
            if (!research::decompress_solid(r->body.subspan(b.at), listing)) {
                continue;
            }
            Reader l{listing, 0};
            std::vector<Entry> es;
            auto at = *covered;
            for (std::uint64_t i = 0; i < *nfiles; ++i) {
                auto e = read_file_info(l, g, h, at);
                if (!e) {
                    break;
                }
                for (const auto &blk : e->blocks) {
                    at += blk.stored;
                }
                es.push_back(std::move(*e));
            }
            if (es.size() != *nfiles || l.at != listing.size() || at != q) {
                continue; // must describe exactly the data between `covered` and itself
            }
            for (std::size_t i = 0; i < es.size(); ++i) {
                if (auto a = add_entry(s, *first + i, std::move(es[i])); !a) {
                    return std::unexpected(a.error());
                }
            }
            s.checkpoints.emplace_back(static_cast<std::uint32_t>(*first),
                                       static_cast<std::uint32_t>(*nfiles));
            s.record_bytes[rec_checkpoint] += r->end - q;
            p = r->end;
            region_start = p;
        } else if (r->type == rec_frame && h.solid() && h.redundancy != red_off) {
            const auto grp = b.get(1), raw = b.get(8), stored = b.get(8), nfiles = b.get(4);
            if (!nfiles || *grp >= g.groups.size() || *raw == 0 ||
                *raw > max_frame_raw || *stored == 0 || *nfiles > max_files) {
                continue;
            }
            std::vector<std::pair<std::uint64_t, Entry>> es;
            for (std::uint64_t i = 0; i < *nfiles; ++i) {
                const auto index = b.get(4);
                auto path = index ? get_text(b, 1) : std::nullopt;
                const auto size = path ? b.get(8) : std::nullopt;
                const auto off = size ? b.get(8) : std::nullopt;
                const auto crc = off ? b.get(4) : std::nullopt;
                if (!crc || *off > *raw || *raw - *off < *size) {
                    break;
                }
                Entry e;
                e.path = std::move(*path);
                e.size = *size;
                e.gen = g.index;
                e.group = static_cast<std::uint8_t>(*grp);
                e.frame = static_cast<std::uint32_t>(s.frames.size());
                e.frame_offset = *off;
                e.file_crc = static_cast<std::uint32_t>(*crc);
                es.emplace_back(*index, std::move(e));
            }
            if (es.size() != *nfiles || b.at != r->body.size()) {
                continue;
            }
            auto avail = std::min<std::uint64_t>(*stored, bytes.size() - r->end);
            if (presence != nullptr) {
                avail = std::min(avail, presence->run(r->end)); // decode only up to a hole
            }
            s.frames.push_back({r->end, *stored, *raw, g.index, static_cast<std::uint8_t>(*grp)});
            s.frame_raw.emplace_back();
            s.frame_produced.push_back(0);
            if (decode && avail > 0) {
                auto &out = s.frame_raw.back();
                out.resize(static_cast<std::size_t>(*raw));
                const auto in = bytes.subspan(r->end, static_cast<std::size_t>(avail));
                const auto codec = g.recipe ? g.recipe->codec : research::solid_zstd;
                if (avail == *stored) {
                    if (research::decompress_solid_codec(codec, in, out)) {
                        s.frame_produced.back() = out.size();
                    }
                } else if (auto n = research::decompress_solid_prefix_codec(codec, in, out)) {
                    s.frame_produced.back() = *n;
                }
            }
            for (auto &[index, e] : es) {
                if (auto a = add_entry(s, index, std::move(e)); !a) {
                    return std::unexpected(a.error());
                }
            }
            s.record_bytes[rec_frame] += r->end - q;
            p = static_cast<std::size_t>(r->end + avail);
            region_start = p;
        }
    }
    return s;
}

// ---- trailer ------------------------------------------------------------------------------------
std::vector<std::byte> trailer_bytes(const Model &m) {
    std::vector<std::byte> t;
    put(t, seedarc::experimental::crc32c(header_of(m.header)),
        4); // the header has no CRC of its own
    put(t, m.entries.size(), 4);
    put(t, m.gens.size(), 4);
    for (const auto &g : m.gens) {
        put(t, g.record_offset, 8);
        put(t, g.files_after, 4);
        put(t, g.body.size(), 4);
        t.insert(t.end(), g.body.begin(), g.body.end());
    }
    if (m.header.explicit_groups()) {
        for (const auto &e : m.entries) {
            t.push_back(static_cast<std::byte>(e.group));
        }
    }
    for (const auto &e : m.entries) {
        put_text(t, e.path);
        put(t, e.size, 8);
        if (m.header.solid()) {
            put(t, e.frame, 4);
            put(t, e.frame_offset, 8);
            put(t, e.file_crc, 4);
        } else {
            put(t, e.blocks.size(), 4);
            for (const auto &b : e.blocks) {
                put(t, b.offset, 8);
                put(t, b.stored, 4);
                t.push_back(static_cast<std::byte>(b.kind));
                put(t, b.crc, 4);
            }
        }
    }
    if (m.header.solid()) {
        put(t, m.frames.size(), 4);
        for (const auto &f : m.frames) {
            put(t, f.offset, 8);
            put(t, f.stored, 8);
            put(t, f.raw, 8);
            put(t, f.gen, 4);
            t.push_back(static_cast<std::byte>(f.group));
        }
    }
    return t;
}
void finish_container(std::vector<std::byte> &out, Model &m) {
    m.trailer_offset = out.size();
    const auto t = trailer_bytes(m);
    out.insert(out.end(), t.begin(), t.end());
    put(out, m.trailer_offset, 8);
    put(out, t.size(), 4);
    put(out, seedarc::experimental::crc32c(t), 4);
    const auto f = as_bytes(footer_magic);
    out.insert(out.end(), f.begin(), f.end());
}

// The footer's view of the trailer.
struct FooterInfo {
    std::uint64_t trailer_offset = 0, trailer_size = 0;
    std::uint32_t trailer_crc = 0;
};
// `footer` is the last footer_bytes of a container of `container_size` bytes.
std::expected<FooterInfo, Error> parse_footer(std::span<const std::byte> footer,
                                              std::uint64_t container_size) {
    if (footer.size() != footer_bytes || container_size < header_bytes + footer_bytes ||
        std::memcmp(footer.data() + footer_bytes - footer_magic.size(), footer_magic.data(),
                    footer_magic.size()) != 0) {
        return std::unexpected(bad("bad footer magic"));
    }
    Reader f{footer, 0};
    const auto toff = f.get(8), tsize = f.get(4), tcrc = f.get(4);
    const auto footer_at = container_size - footer_bytes;
    if (!tcrc || *toff < header_bytes || *toff > footer_at || footer_at - *toff != *tsize) {
        return std::unexpected(bad("bad footer"));
    }
    return FooterInfo{*toff, *tsize, static_cast<std::uint32_t>(*tcrc)};
}

// ADR-0006 §2.4: a recipe generation's frames must be exactly what the writer's greedy rule
// gives for its recipe (per group, files in generation order, flush before a file that does not
// fit; an all-empty frame holds one zero byte). Otherwise the label could disagree with the data.
std::expected<void, Error> check_recipe_frames(const Model &m, const Gen &g) {
    const auto limit = recipe_frame_limit(*g.recipe);
    std::vector<std::size_t> own;
    for (std::size_t i = 0; i < m.frames.size(); ++i) {
        if (m.frames[i].gen == g.index) {
            own.push_back(i);
        }
    }
    const auto mismatch = [] {
        return std::unexpected(bad("solid frames do not follow the generation's recipe"));
    };
    // ADR-0008 dedup: every file counts towards the frame budget; only non-alias files add frame
    // bytes and must be laid out in order. An alias is any entry not at the next in-order position;
    // it must repeat (frame, offset, size, CRC) of an earlier non-alias, non-empty entry of the
    // same generation and group. Without dedup no alias is allowed.
    std::size_t next = 0;
    for (std::size_t grp = 0; grp < g.groups.size(); ++grp) {
        std::uint64_t joined = 0, budget = 0;
        bool any = false;
        std::map<std::pair<std::uint32_t, std::uint64_t>, const Entry *> placed;
        const auto flush = [&] {
            if (!any) {
                return true;
            }
            if (next >= own.size()) {
                return false;
            }
            const auto &f = m.frames[own[next]];
            if (f.group != grp || f.raw != std::max<std::uint64_t>(joined, 1)) {
                return false;
            }
            ++next;
            joined = 0;
            budget = 0;
            any = false;
            return true;
        };
        for (std::uint32_t i = g.first_file; i < g.files_after; ++i) {
            const auto &e = m.entries[i];
            if (e.group != grp) {
                continue;
            }
            if (e.size > limit || (e.size > limit - budget && !flush())) {
                return mismatch();
            }
            if (next >= own.size()) {
                return mismatch();
            }
            any = true;
            budget += e.size;
            if (e.frame == own[next] && e.frame_offset == joined) {
                if (e.size > 0) {
                    placed.emplace(std::pair{e.frame, e.frame_offset}, &e);
                }
                joined += e.size;
                continue;
            }
            const auto first = placed.find({e.frame, e.frame_offset});
            if (!g.recipe->dedup || e.size == 0 || first == placed.end() || first->second->size != e.size ||
                first->second->file_crc != e.file_crc) {
                return mismatch();
            }
        }
        if (!flush()) {
            return mismatch();
        }
    }
    if (next != own.size()) {
        return mismatch();
    }
    return {};
}

// A GEN record fetched from wherever the container lives (offsets absolute).
struct GenRecord {
    std::vector<std::byte> body;
    std::uint64_t end = 0;
};
using GenFetch = std::function<std::optional<GenRecord>(std::uint64_t offset)>;

// Interprets a checksum-verified trailer. Every GEN record is fetched at the offset the trailer
// names and must equal its trailer copy; dictionaries are checked inside `dict_bytes` (the
// container prefix before the trailer; an empty span when only an index was read, which fails
// closed for any generation that has dictionaries). This step needs no payload bytes; parse()
// adds the cross-check against a scan of every record.
std::expected<Model, Error> parse_index(const Header &header, std::uint32_t header_crc,
                                        std::span<const std::byte> trailer,
                                        std::uint64_t trailer_offset, const GenFetch &fetch_gen,
                                        std::span<const std::byte> dict_bytes,
                                        double *manifest_ms = nullptr) {
    Model m;
    m.header = header;
    m.trailer_offset = trailer_offset;
    const auto &h = m.header;
    Reader t{trailer, 0};
    const auto hcrc = t.get(4), count = t.get(4), ngens = t.get(4);
    if (!ngens || *hcrc != header_crc || *count > max_files || *ngens == 0 ||
        *ngens > max_generations) {
        return std::unexpected(bad("bad counts"));
    }
    std::uint32_t prev_after = 0;
    for (std::uint64_t i = 0; i < *ngens; ++i) {
        const auto off = t.get(8), after = t.get(4), blen = t.get(4);
        if (!blen || *blen > max_record_body || trailer.size() - t.at < *blen) {
            return std::unexpected(bad("bad generation entry"));
        }
        const auto copy = trailer.subspan(t.at, static_cast<std::size_t>(*blen));
        t.at += static_cast<std::size_t>(*blen);
        const auto rec = *off < trailer_offset ? fetch_gen(*off) : std::nullopt;
        if (!rec || !std::ranges::equal(rec->body, copy)) {
            return std::unexpected(bad("generation record differs from its trailer copy"));
        }
        auto g = parse_gen_body(copy, h);
        if (!g) {
            return std::unexpected(g.error());
        }
        if (g->index != i || g->first_file != prev_after || *after <= prev_after ||
            *after > *count || (!m.gens.empty() && *off < m.data_start.back())) {
            return std::unexpected(bad("bad generation order"));
        }
        const auto ds = check_gen_dicts(dict_bytes, *g, rec->end, &m.gens);
        if (!ds) {
            return std::unexpected(ds.error());
        }
        g->files_after = static_cast<std::uint32_t>(*after);
        prev_after = g->files_after;
        m.data_start.push_back(*ds);
        m.gens.push_back(std::move(*g));
    }
    if (prev_after != *count) {
        return std::unexpected(bad("generations do not cover every file"));
    }
    const auto region_end = [&](std::uint32_t gi) {
        return gi + 1 < m.gens.size() ? m.gens[gi + 1].record_offset : m.trailer_offset;
    };
    std::vector<std::uint8_t> explicit_groups;
    if (h.explicit_groups()) {
        if (trailer.size() - t.at < *count) {
            return std::unexpected(bad("truncated group bytes"));
        }
        for (std::uint64_t i = 0; i < *count; ++i) {
            explicit_groups.push_back(static_cast<std::uint8_t>(*t.get(1)));
        }
    }
    std::uint64_t total = 0;
    std::vector<std::string> paths;
    std::uint32_t gi = 0;
    for (std::uint64_t i = 0; i < *count; ++i) {
        while (i >= m.gens[gi].files_after) {
            ++gi;
        }
        const auto &g = m.gens[gi];
        Entry e;
        auto path = get_text(t, 1);
        const auto size = path ? t.get(8) : std::nullopt;
        if (!size || *size > max_input) {
            return std::unexpected(bad("bad entry"));
        }
        e.path = std::move(*path);
        e.size = *size;
        e.gen = gi;
        total += e.size;
        if (h.solid()) {
            const auto frame = t.get(4), off = t.get(8), crc = t.get(4);
            if (!crc) {
                return std::unexpected(bad("bad solid entry"));
            }
            e.frame = static_cast<std::uint32_t>(*frame);
            e.frame_offset = *off;
            e.file_crc = static_cast<std::uint32_t>(*crc);
        } else {
            e.group = h.seed() ? group_of(g, e.path, e.size)
                               : explicit_groups[static_cast<std::size_t>(i)];
            if (e.group >= g.groups.size()) {
                return std::unexpected(bad("bad file group"));
            }
            const bool has_dict = g.groups[e.group].dict_size != 0;
            const auto blocks = t.get(4);
            if (!blocks || *blocks != (*size + chunk - 1) / chunk) {
                return std::unexpected(bad("bad block count"));
            }
            for (std::uint64_t b = 0; b < *blocks; ++b) {
                const auto off = t.get(8), stored = t.get(4), kind = t.get(1), crc = t.get(4);
                const auto raw = std::min<std::uint64_t>(chunk, *size - b * chunk);
                if (!crc || *kind > kind_zstd_dict || *stored == 0 || *stored > raw ||
                    (*kind == kind_store) != (*stored == raw) ||
                    (*kind == kind_zstd_dict && !has_dict) || (*kind == kind_zstd && has_dict) ||
                    *off < m.data_start[gi] || *off > region_end(gi) ||
                    region_end(gi) - *off < *stored) {
                    return std::unexpected(bad("bad block"));
                }
                e.blocks.push_back({*off, static_cast<std::uint32_t>(*stored),
                                    static_cast<std::uint8_t>(*kind),
                                    static_cast<std::uint32_t>(*crc)});
            }
        }
        paths.push_back(e.path);
        m.entries.push_back(std::move(e));
    }
    if (total > max_input) {
        return std::unexpected(Error{ErrorCode::resource_limit, "container content too large"});
    }
    if (h.solid()) {
        const auto nframes = t.get(4);
        if (!nframes || *nframes > max_files) {
            return std::unexpected(bad("bad frame count"));
        }
        for (std::uint64_t i = 0; i < *nframes; ++i) {
            const auto off = t.get(8), stored = t.get(8), raw = t.get(8), fgen = t.get(4),
                       group = t.get(1);
            if (!group || *fgen >= m.gens.size() || *group >= m.gens[*fgen].groups.size() ||
                *off < m.data_start[*fgen] ||
                *off > region_end(static_cast<std::uint32_t>(*fgen)) ||
                region_end(static_cast<std::uint32_t>(*fgen)) - *off < *stored || *stored == 0 ||
                *raw == 0 || *raw > max_frame_raw) {
                return std::unexpected(bad("bad frame"));
            }
            m.frames.push_back({*off, *stored, *raw, static_cast<std::uint32_t>(*fgen),
                                static_cast<std::uint8_t>(*group)});
        }
        for (auto &e : m.entries) {
            if (e.frame >= m.frames.size() || m.frames[e.frame].gen != e.gen ||
                e.frame_offset > m.frames[e.frame].raw ||
                m.frames[e.frame].raw - e.frame_offset < e.size) {
                return std::unexpected(bad("solid entry outside its frame"));
            }
            e.group = m.frames[e.frame].group;
            if (h.revision == 5 && e.group != group_of(m.gens[e.gen], e.path, e.size)) {
                return std::unexpected(bad("solid strategy and frame group disagree"));
            }
        }
        for (const auto &g : m.gens) {
            if (g.recipe) {
                if (auto ok = check_recipe_frames(m, g); !ok) {
                    return std::unexpected(ok.error());
                }
            }
        }
    }
    if (t.at != trailer.size()) {
        return std::unexpected(bad("trailing bytes in the trailer"));
    }
    const auto manifest_start = wall_ms();
    if (auto ok = check_manifest(paths); !ok) {
        return std::unexpected(ok.error());
    }
    if (manifest_ms != nullptr) {
        *manifest_ms = wall_ms() - manifest_start;
    }
    return m;
}

// Bounded, strict parse of the whole container (footer -> trailer -> records), cross-checked
// against a scan of the records; any difference is an error.
std::expected<Model, Error> parse(std::span<const std::byte> bytes) {
    auto header = parse_header(bytes);
    if (!header) {
        return std::unexpected(header.error());
    }
    if (bytes.size() < header_bytes + footer_bytes) {
        return std::unexpected(bad("bad footer magic"));
    }
    const auto footer = parse_footer(bytes.last(footer_bytes), bytes.size());
    if (!footer) {
        return std::unexpected(footer.error());
    }
    const auto trailer = bytes.subspan(static_cast<std::size_t>(footer->trailer_offset),
                                       static_cast<std::size_t>(footer->trailer_size));
    if (seedarc::experimental::crc32c(trailer) != footer->trailer_crc) {
        return std::unexpected(bad("trailer checksum mismatch"));
    }
    const auto body_bytes = bytes.first(static_cast<std::size_t>(footer->trailer_offset));
    const GenFetch fetch = [&](std::uint64_t off) -> std::optional<GenRecord> {
        const auto rec = read_record(body_bytes, static_cast<std::size_t>(off));
        if (!rec || rec->type != rec_gen) {
            return std::nullopt;
        }
        return GenRecord{{rec->body.begin(), rec->body.end()}, rec->end};
    };
    auto parsed = parse_index(*header, seedarc::experimental::crc32c(bytes.first(header_bytes)),
                              trailer, footer->trailer_offset, fetch, body_bytes);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    auto &m = *parsed;
    const auto &h = m.header;
    // Cross-check: the records must describe exactly what the trailer says.
    auto s = scan(body_bytes, h, false);
    if (!s) {
        return std::unexpected(s.error());
    }
    if (s->gens.size() != m.gens.size()) {
        return std::unexpected(bad("generation records differ from the trailer"));
    }
    for (const auto &g : m.gens) {
        const auto it = s->gens.find(g.index);
        if (it == s->gens.end() || it->second.body != g.body) {
            return std::unexpected(bad("generation records differ from the trailer"));
        }
    }
    const bool listed = h.redundancy != red_off;
    if (s->entries.size() != (listed ? m.entries.size() : 0) ||
        s->frames != (listed ? m.frames : std::vector<Frame>{})) {
        return std::unexpected(bad("redundancy records differ from the trailer"));
    }
    for (const auto &[index, e] : s->entries) {
        if (index >= m.entries.size() || m.entries[index] != e) {
            return std::unexpected(bad("redundancy records differ from the trailer"));
        }
    }
    if (h.redundancy == red_checkpoint && !h.solid()) {
        // Checkpoints must sit where the header's checkpoint size puts them. (Solid mode has
        // one FRM record per frame at every level but off.)
        std::vector<std::pair<std::uint32_t, std::uint32_t>> want;
        for (const auto &g : m.gens) {
            std::uint32_t first = g.first_file;
            std::uint64_t pending = 0;
            for (std::uint32_t i = g.first_file; i < g.files_after; ++i) {
                for (const auto &b : m.entries[i].blocks) {
                    pending += b.stored;
                }
                if (pending >= h.checkpoint_bytes || i + 1 == g.files_after) {
                    want.emplace_back(first, i + 1 - first);
                    first = i + 1;
                    pending = 0;
                }
            }
        }
        if (s->checkpoints != want) {
            return std::unexpected(bad("checkpoints are not where the header puts them"));
        }
    }
    m.record_bytes = s->record_bytes;
    return std::move(*parsed);
}

// ---- decoding -----------------------------------------------------------------------------------
// Dictionary decoders by (offset, size): one per distinct dictionary in the container.
using DecoderCache = std::map<std::pair<std::uint64_t, std::uint32_t>, research::DictionaryDecoder>;

template <class Consumer>
std::expected<void, Error> visit_blocks(const Entry &e, const GenGroup &grp,
                                        std::span<const std::byte> bytes, DecoderCache &cache,
                                        Consumer &&consume) {
    research::DictionaryDecoder *dict = nullptr;
    if (grp.dict_size != 0) {
        const auto key = std::pair{grp.dict_offset, grp.dict_size};
        auto it = cache.find(key);
        if (it == cache.end()) {
            auto created = research::DictionaryDecoder::create(
                bytes.subspan(static_cast<std::size_t>(grp.dict_offset), grp.dict_size));
            if (!created) {
                return std::unexpected(created.error());
            }
            it = cache.emplace(key, std::move(*created)).first;
        }
        dict = &it->second;
    }
    codec::Decoder plain;
    // Only the exact decoded destination is read below; codecs must fill it before success.
    std::array<std::byte, chunk> scratch;
    std::size_t at = 0;
    for (const auto &b : e.blocks) {
        if (at >= e.size) {
            return std::unexpected(bad("too many blocks for file"));
        }
        const auto raw = std::min<std::size_t>(chunk, static_cast<std::size_t>(e.size) - at);
        if (b.offset > bytes.size() || bytes.size() - b.offset < b.stored) {
            return std::unexpected(bad("block outside the container"));
        }
        const auto in = bytes.subspan(static_cast<std::size_t>(b.offset), b.stored);
        const auto dst = std::span{scratch}.first(raw);
        if (b.kind == kind_store) {
            if (in.size() != dst.size()) {
                return std::unexpected(bad("stored block size mismatch"));
            }
            std::ranges::copy(in, dst.begin());
        } else if (b.kind == kind_zstd) {
            if (auto r = plain.decode(codec::zstd_id, in, dst); !r) {
                return std::unexpected(r.error());
            }
        } else if (dict == nullptr) {
            return std::unexpected(bad("dictionary block without a dictionary"));
        } else if (auto r = dict->decode(in, dst); !r) {
            return std::unexpected(r.error());
        }
        if (seedarc::experimental::crc32c(dst) != b.crc) {
            return std::unexpected(bad("block crc mismatch"));
        }
        if (auto r = consume(at, std::span<const std::byte>{dst}); !r) {
            return r;
        }
        at += raw;
    }
    if (at != e.size) {
        return std::unexpected(bad("file blocks do not cover its size"));
    }
    // Also visit empty files, and let a consumer observe end-of-file without retaining bytes.
    return consume(at, std::span<const std::byte>{});
}

std::expected<std::vector<std::byte>, Error> decode_blocks(const Entry &e, const GenGroup &grp,
                                                           std::span<const std::byte> bytes,
                                                           DecoderCache &cache) {
    std::vector<std::byte> file(static_cast<std::size_t>(e.size));
    auto r = visit_blocks(
        e, grp, bytes, cache,
        [&](std::size_t at, std::span<const std::byte> raw) -> std::expected<void, Error> {
            std::ranges::copy(raw, file.begin() + static_cast<std::ptrdiff_t>(at));
            return {};
        });
    if (!r) {
        return std::unexpected(r.error());
    }
    return file;
}

// The codec of a solid frame: its generation's recipe, or Zstd for mapping revision 2.
std::uint32_t gen_codec(const Gen &g) { return g.recipe ? g.recipe->codec : research::solid_zstd; }
std::uint32_t frame_codec(const Model &m, const Frame &f) { return gen_codec(m.gens[f.gen]); }

std::expected<std::vector<std::byte>, Error> decode_frame(const Model &m, const Frame &f,
                                                          std::span<const std::byte> bytes) {
    std::vector<std::byte> raw(static_cast<std::size_t>(f.raw));
    const auto frame =
        bytes.subspan(static_cast<std::size_t>(f.offset), static_cast<std::size_t>(f.stored));
    if (auto r = research::decompress_solid_codec(frame_codec(m, f), frame, raw); !r) {
        return std::unexpected(r.error());
    }
    return raw;
}

// Visit every block/frame and validate every CRC. A subset comparison must still validate old
// payloads: append is not permitted to copy corrupt old content into a purportedly valid output.
template <class Consumer>
std::expected<void, Error> visit_decoded(const Model &m, std::span<const std::byte> bytes,
                                         Consumer &&consume) {
    if (m.header.solid()) {
        std::vector<std::vector<const Entry *>> entries(m.frames.size());
        for (const auto &e : m.entries) {
            if (e.frame >= entries.size()) {
                return std::unexpected(bad("file frame index out of range"));
            }
            entries[e.frame].push_back(&e);
        }
        for (std::size_t frame = 0; frame < m.frames.size(); ++frame) {
            auto r = decode_frame(m, m.frames[frame], bytes);
            if (!r) {
                return std::unexpected(r.error());
            }
            for (const auto *e : entries[frame]) {
                if (e->frame_offset > r->size() || e->size > r->size() - e->frame_offset) {
                    return std::unexpected(bad("file outside decoded frame"));
                }
                const auto view = std::span<const std::byte>{*r}.subspan(
                    static_cast<std::size_t>(e->frame_offset), static_cast<std::size_t>(e->size));
                if (seedarc::experimental::crc32c(view) != e->file_crc) {
                    return std::unexpected(bad("file crc mismatch"));
                }
                if (auto checked = consume(*e, 0, view); !checked) {
                    return checked;
                }
            }
        }
        return {};
    }
    for (DecoderCache cache; const auto &e : m.entries) {
        auto r = visit_blocks(
            e, m.gens[e.gen].groups[e.group], bytes, cache,
            [&](std::size_t at, std::span<const std::byte> raw) { return consume(e, at, raw); });
        if (!r) {
            return r;
        }
    }
    return {};
}

std::expected<std::map<std::string, std::vector<std::byte>>, Error>
decode_all(const Model &m, std::span<const std::byte> bytes) {
    std::map<std::string, std::vector<std::byte>> out;
    auto r = visit_decoded(m, bytes,
                           [&](const Entry &e, std::size_t at,
                               std::span<const std::byte> raw) -> std::expected<void, Error> {
                               auto [it, inserted] = out.try_emplace(e.path);
                               if (inserted) {
                                   it->second.resize(static_cast<std::size_t>(e.size));
                               }
                               std::ranges::copy(raw, it->second.begin() +
                                                          static_cast<std::ptrdiff_t>(at));
                               return {};
                           });
    if (!r) {
        return std::unexpected(r.error());
    }
    return out;
}

std::expected<void, Error> byte_exact(const std::vector<InputFile> &files, const Model &m,
                                      std::span<const std::byte> bytes, bool require_all = true) {
    if (require_all && m.entries.size() != files.size()) {
        return std::unexpected(bad("file count differs from input"));
    }
    std::map<std::string_view, const std::vector<std::byte> *, std::less<>> expected;
    for (const auto &f : files) {
        if (!expected.emplace(f.path, &f.data).second) {
            return std::unexpected(bad("duplicate comparison file"));
        }
    }
    std::set<std::string_view> seen;
    auto r = visit_decoded(
        m, bytes,
        [&](const Entry &e, std::size_t at,
            std::span<const std::byte> raw) -> std::expected<void, Error> {
            const auto it = expected.find(e.path);
            if (it == expected.end()) {
                return require_all ? std::unexpected(bad("file missing from comparison"))
                                   : std::expected<void, Error>{};
            }
            const auto &data = *it->second;
            if (data.size() != e.size || at > data.size() || raw.size() > data.size() - at ||
                !std::ranges::equal(raw, std::span{data}.subspan(at, raw.size()))) {
                return std::unexpected(bad("round trip mismatch"));
            }
            seen.insert(it->first);
            return {};
        });
    if (!r) {
        return r;
    }
    if (seen.size() != expected.size()) {
        return std::unexpected(bad("comparison file missing from archive"));
    }
    return {};
}

// ---- recovery from a prefix or a damaged file ---------------------------------------------------
struct Recovery {
    bool header_ok = false;
    std::map<std::string, std::vector<std::byte>> files;
    std::uint64_t from_trailer = 0, from_records = 0, unreadable = 0;
};
std::expected<Recovery, Error> recover(std::span<const std::byte> bytes,
                                       const Presence *presence = nullptr) {
    Recovery out;
    auto header = parse_header(bytes);
    if (!header) {
        return out; // not even the header: nothing can be read
    }
    if (header->revision == 5) {
        return std::unexpected(bad("recovery is not implemented for solid strategy revision 5"));
    }
    out.header_ok = true;
    std::uint64_t total = 0;
    const auto add = [&](const std::string &path,
                         std::vector<std::byte> data) -> std::expected<bool, Error> {
        const auto it = out.files.find(path);
        if (it != out.files.end()) {
            if (it->second != data) {
                return std::unexpected(bad("conflicting copies of a recovered file"));
            }
            return false;
        }
        total += data.size();
        if (total > max_input) {
            return std::unexpected(Error{ErrorCode::resource_limit, "recovery too large"});
        }
        out.files.emplace(path, std::move(data));
        return true;
    };
    // 1. The newest trailer that is intact in this prefix (the current one, or a superseded one
    //    from an earlier generation).
    const auto fm = as_bytes(footer_magic);
    std::vector<std::size_t> footers;
    for (auto it = bytes.begin();
         (it = std::search(it, bytes.end(), fm.begin(), fm.end())) != bytes.end(); ++it) {
        footers.push_back(static_cast<std::size_t>(it - bytes.begin()));
    }
    for (auto f = footers.rbegin(); f != footers.rend(); ++f) {
        auto m = parse(bytes.first(*f + footer_magic.size()));
        if (!m) {
            continue;
        }
        std::vector<std::optional<std::vector<std::byte>>> frames(m->frames.size());
        for (DecoderCache cache; const auto &e : m->entries) {
            std::expected<std::vector<std::byte>, Error> data = std::unexpected(bad("unread"));
            if (m->header.solid()) {
                auto &raw = frames[e.frame];
                if (!raw) {
                    const auto &fr = m->frames[e.frame];
                    raw = std::vector<std::byte>{};
                    if (covered(presence, fr.offset, fr.stored)) {
                        if (auto r = decode_frame(*m, fr, bytes)) {
                            raw = std::move(*r);
                        }
                    }
                }
                const auto view = std::span{*raw};
                if (view.size() >= e.frame_offset + e.size) {
                    const auto cut = view.subspan(static_cast<std::size_t>(e.frame_offset),
                                                  static_cast<std::size_t>(e.size));
                    if (seedarc::experimental::crc32c(cut) == e.file_crc) {
                        data = std::vector<std::byte>{cut.begin(), cut.end()};
                    }
                }
            } else if (blocks_present(presence, e.blocks)) {
                data = decode_blocks(e, m->gens[e.gen].groups[e.group], bytes, cache);
            }
            if (!data) {
                ++out.unreadable;
                continue;
            }
            auto added = add(e.path, std::move(*data));
            if (!added) {
                return std::unexpected(added.error());
            }
            out.from_trailer += *added ? 1 : 0;
        }
        break;
    }
    // 2. Every valid record, front to back.
    auto s = scan(bytes, *header, true, presence);
    if (!s) {
        return std::unexpected(s.error());
    }
    for (DecoderCache cache; const auto &e : s->entries | std::views::values) {
        std::expected<std::vector<std::byte>, Error> data = std::unexpected(bad("unread"));
        if (header->solid()) {
            const auto &raw = s->frame_raw[e.frame];
            if (s->frame_produced[e.frame] >= e.frame_offset + e.size) {
                const auto cut = std::span{raw}.subspan(static_cast<std::size_t>(e.frame_offset),
                                                        static_cast<std::size_t>(e.size));
                if (seedarc::experimental::crc32c(cut) == e.file_crc) {
                    data = std::vector<std::byte>{cut.begin(), cut.end()};
                }
            }
        } else if (blocks_present(presence, e.blocks)) {
            data = decode_blocks(e, s->gens.at(e.gen).groups[e.group], bytes, cache);
        }
        if (!data) {
            ++out.unreadable;
            continue;
        }
        auto added = add(e.path, std::move(*data));
        if (!added) {
            return std::unexpected(added.error());
        }
        out.from_records += *added ? 1 : 0;
    }
    std::vector<std::string> paths;
    for (const auto &path : out.files | std::views::keys) {
        paths.push_back(path);
    }
    if (auto ok = check_manifest(paths); !ok) {
        return std::unexpected(ok.error());
    }
    return out;
}

// ---- encoding and layout ------------------------------------------------------------------------
// Solid frame encoder workers (`--jobs N`, 1-16; default 1). Archive bytes are the same for every
// value; peak memory grows with it (each worker holds one joined frame plus its encoder state).
constexpr std::uint64_t max_encode_jobs = 16;
std::size_t encode_jobs = 1;

// One generation, encoded but not yet laid out.
struct Pending {
    Gen gen; // grouping, plan, groups (inherited triples; new ones get their offset at layout)
    std::vector<std::vector<std::byte>> new_dicts; // per group; non-empty = introduced here
    std::vector<Entry> entries;                    // block offsets relative to the payload
    std::vector<std::vector<std::byte>> payloads;  // block mode: per file; solid: per frame
    std::vector<Frame> frames;                     // solid: offsets set at layout
};

std::expected<void, Error> encode_pending(Pending &p, const Header &h,
                                          const std::vector<InputFile> &files,
                                          const std::vector<std::uint8_t> &groups,
                                          const std::vector<std::span<const std::byte>> &dicts,
                                          std::uint64_t frame_limit = research::max_solid_bytes) {
    if (frame_limit == 0 || frame_limit > research::max_solid_bytes) {
        return std::unexpected(bad("invalid solid frame limit"));
    }
    p.entries.resize(files.size());
    for (std::size_t i = 0; i < files.size(); ++i) {
        p.entries[i].path = files[i].path;
        p.entries[i].size = files[i].data.size();
        p.entries[i].group = groups[i];
    }
    if (!h.solid()) {
        for (std::size_t i = 0; i < files.size(); ++i) {
            std::vector<std::byte> payload;
            auto blocks = encode_blocks(files[i], dicts[groups[i]], payload);
            if (!blocks) {
                return std::unexpected(blocks.error());
            }
            p.entries[i].blocks = std::move(*blocks);
            p.payloads.push_back(std::move(payload));
        }
        return {};
    }
    // 1. Layout, serial: which files go into which frame (cut as before: path order within a
    //    group, close a frame when the next file does not fit). Offsets and CRCs are final here.
    struct FrameInput {
        std::uint8_t group = 0;
        std::vector<std::size_t> members; // file indices, in frame order
        std::uint64_t raw = 0;
    };
    //    ADR-0008 dedup recipes: a non-empty file equal to an earlier file of the same group (same
    //    size and CRC32C, then compared byte for byte) becomes an alias of its first copy. It still
    //    counts towards the frame budget, so boundaries stay where they would be without dedup.
    std::vector<FrameInput> plan;
    const auto base = p.frames.size();
    const bool dedup = p.gen.recipe && p.gen.recipe->dedup;
    for (std::size_t g = 0; g < p.gen.groups.size(); ++g) {
        bool open = false;
        std::uint64_t budget = 0;
        std::map<std::pair<std::uint64_t, std::uint32_t>, std::vector<std::size_t>> firsts;
        for (std::size_t i = 0; i < files.size(); ++i) {
            if (groups[i] != g) {
                continue;
            }
            const auto size = files[i].data.size();
            if (size > frame_limit) {
                return std::unexpected(Error{ErrorCode::resource_limit,
                                             "file exceeds the requested solid frame limit"});
            }
            if (!open || size > frame_limit - budget) {
                plan.push_back({static_cast<std::uint8_t>(g), {}, 0});
                open = true;
                budget = 0;
            }
            budget += size;
            auto &frame = plan.back();
            auto &e = p.entries[i];
            e.file_crc = seedarc::experimental::crc32c(files[i].data);
            if (dedup && size > 0) {
                auto &bucket = firsts[{size, e.file_crc}];
                const auto same = std::ranges::find_if(
                    bucket, [&](std::size_t j) { return files[j].data == files[i].data; });
                if (same != bucket.end()) {
                    e.frame = p.entries[*same].frame;
                    e.frame_offset = p.entries[*same].frame_offset;
                    continue;
                }
                bucket.push_back(i);
            }
            e.frame = static_cast<std::uint32_t>(base + plan.size() - 1);
            e.frame_offset = frame.raw;
            frame.members.push_back(i);
            frame.raw += size;
        }
    }
    // 2. Compression: every frame is independent, so frames may be encoded by `encode_jobs`
    //    workers. Each result lands in its own slot and is appended in frame order below, so the
    //    archive bytes do not depend on the worker count; the first failing frame's error wins.
    std::vector<std::expected<std::vector<std::byte>, Error>> encoded(
        plan.size(), std::unexpected(Error{ErrorCode::io_error, "frame not encoded"}));
    const auto encode_one = [&](std::size_t f) {
        try {
            std::vector<std::byte> joined;
            joined.reserve(static_cast<std::size_t>(std::max<std::uint64_t>(plan[f].raw, 1)));
            for (const auto i : plan[f].members) {
                joined.insert(joined.end(), files[i].data.begin(), files[i].data.end());
            }
            if (joined.empty()) {
                joined.push_back(std::byte{0}); // retain a frame for all-empty files
            }
            // Recipe generations use their own codec and level; revision 2 keeps Zstd 9.
            encoded[f] = p.gen.recipe ? research::compress_solid_codec(p.gen.recipe->codec,
                                                                       p.gen.recipe->level, joined,
                                                                       p.gen.recipe->extreme)
                                      : research::compress_solid(joined, level);
        } catch (const std::bad_alloc &) {
            encoded[f] = std::unexpected(Error{ErrorCode::resource_limit, "out of memory encoding a frame"});
        }
    };
    const auto workers = std::min<std::size_t>(encode_jobs, plan.size());
    if (workers <= 1) {
        for (std::size_t f = 0; f < plan.size(); ++f) {
            encode_one(f);
            if (!encoded[f]) {
                break; // serial: stop at the first failure, as before
            }
        }
    } else {
        std::atomic<std::size_t> next{0};
        std::atomic<bool> failed{false};
        std::vector<std::jthread> pool;
        pool.reserve(workers);
        for (std::size_t w = 0; w < workers; ++w) {
            pool.emplace_back([&] {
                for (auto f = next.fetch_add(1); f < plan.size() && !failed.load(); f = next.fetch_add(1)) {
                    encode_one(f);
                    if (!encoded[f]) {
                        failed.store(true);
                    }
                }
            });
        }
        pool.clear(); // joins
    }
    for (std::size_t f = 0; f < plan.size(); ++f) {
        if (!encoded[f]) {
            return std::unexpected(encoded[f].error());
        }
        const auto raw = std::max<std::uint64_t>(plan[f].raw, 1);
        p.frames.push_back({0, encoded[f]->size(), raw, 0, plan[f].group});
        p.payloads.push_back(std::move(*encoded[f]));
    }
    return {};
}

std::expected<void, Error> write_checkpoint(std::vector<std::byte> &out, const Model &m,
                                            std::uint32_t gen, std::size_t first,
                                            std::uint64_t covered) {
    std::vector<std::byte> listing;
    for (std::size_t i = first; i < m.entries.size(); ++i) {
        put_file_info(listing, m.entries[i], m.header.explicit_groups());
    }
    if (listing.size() > max_record_body) {
        return std::unexpected(Error{ErrorCode::resource_limit, "checkpoint listing too large"});
    }
    auto packed = research::compress_solid(listing, level);
    if (!packed) {
        return std::unexpected(packed.error());
    }
    std::vector<std::byte> body;
    put(body, out.size(), 8);
    put(body, gen, 4);
    put(body, first, 4);
    put(body, m.entries.size() - first, 4);
    put(body, covered, 8);
    put(body, listing.size(), 4);
    body.insert(body.end(), packed->begin(), packed->end());
    if (body.size() > max_record_body) {
        return std::unexpected(Error{ErrorCode::resource_limit, "checkpoint record too large"});
    }
    put_record(out, rec_checkpoint, body);
    return {};
}

// Lays one generation out at the end of `out` and adds it to `m`.
std::expected<void, Error> layout(std::vector<std::byte> &out, Model &m, Pending p) {
    const auto &h = m.header;
    Gen g = std::move(p.gen);
    g.index = static_cast<std::uint32_t>(m.gens.size());
    g.first_file = static_cast<std::uint32_t>(m.entries.size());
    g.record_offset = out.size();
    if (g.index >= max_generations || m.entries.size() + p.entries.size() > max_files) {
        return std::unexpected(Error{ErrorCode::resource_limit, "too many generations or files"});
    }
    const auto record_end = g.record_offset + record_overhead + gen_body(g).size();
    auto cursor = record_end;
    for (std::size_t i = 0; i < g.groups.size(); ++i) {
        if (!p.new_dicts[i].empty()) {
            g.groups[i] = {cursor, static_cast<std::uint32_t>(p.new_dicts[i].size()),
                           seedarc::experimental::crc32c(p.new_dicts[i])};
            cursor += p.new_dicts[i].size();
        }
    }
    g.body = gen_body(g);
    put_record(out, rec_gen, g.body);
    for (const auto &d : p.new_dicts) {
        out.insert(out.end(), d.begin(), d.end());
    }
    m.data_start.push_back(out.size());
    if (!h.solid()) {
        std::size_t pending_first = m.entries.size();
        std::uint64_t pending_start = out.size(), pending_bytes = 0;
        for (std::size_t i = 0; i < p.entries.size(); ++i) {
            Entry e = std::move(p.entries[i]);
            e.gen = g.index;
            const auto &payload = p.payloads[i];
            if (h.redundancy == red_file) {
                std::vector<std::byte> body;
                put(body, out.size(), 8);
                put(body, g.index, 4);
                put(body, m.entries.size(), 4);
                put_file_info(body, e, h.explicit_groups());
                if (body.size() > max_record_body) {
                    return std::unexpected(
                        Error{ErrorCode::resource_limit, "file record too large"});
                }
                put_record(out, rec_file, body);
            }
            const auto base = out.size();
            for (auto &b : e.blocks) {
                b.offset += base;
            }
            out.insert(out.end(), payload.begin(), payload.end());
            m.entries.push_back(std::move(e));
            if (h.redundancy == red_checkpoint) {
                pending_bytes += payload.size();
                if (pending_bytes >= h.checkpoint_bytes || i + 1 == p.entries.size()) {
                    if (auto r = write_checkpoint(out, m, g.index, pending_first, pending_start);
                        !r) {
                        return std::unexpected(r.error());
                    }
                    pending_first = m.entries.size();
                    pending_start = out.size();
                    pending_bytes = 0;
                }
            }
        }
    } else {
        const auto frame_base = static_cast<std::uint32_t>(m.frames.size());
        for (std::size_t fi = 0; fi < p.frames.size(); ++fi) {
            Frame f = p.frames[fi];
            f.gen = g.index;
            if (h.redundancy != red_off) {
                std::vector<std::byte> body;
                put(body, out.size(), 8);
                put(body, g.index, 4);
                body.push_back(static_cast<std::byte>(f.group));
                put(body, f.raw, 8);
                put(body, f.stored, 8);
                std::vector<std::byte> list;
                std::uint32_t n = 0;
                for (std::size_t i = 0; i < p.entries.size(); ++i) {
                    const auto &e = p.entries[i];
                    if (e.frame != fi) {
                        continue;
                    }
                    ++n;
                    put(list, g.first_file + i, 4);
                    put_text(list, e.path);
                    put(list, e.size, 8);
                    put(list, e.frame_offset, 8);
                    put(list, e.file_crc, 4);
                }
                put(body, n, 4);
                body.insert(body.end(), list.begin(), list.end());
                if (body.size() > max_record_body) {
                    return std::unexpected(
                        Error{ErrorCode::resource_limit, "frame record too large"});
                }
                put_record(out, rec_frame, body);
            }
            f.offset = out.size();
            out.insert(out.end(), p.payloads[fi].begin(), p.payloads[fi].end());
            m.frames.push_back(f);
        }
        for (auto &e : p.entries) {
            e.gen = g.index;
            e.frame += frame_base;
            m.entries.push_back(std::move(e));
        }
    }
    g.files_after = static_cast<std::uint32_t>(m.entries.size());
    m.gens.push_back(std::move(g));
    return {};
}

// A new generation's template with its own plan: grouping from the plan's rule on `files`, and
// (block modes) dictionaries trained on them.
std::expected<Pending, Error> fresh_pending(const Header &h, const Plan &plan,
                                            const std::vector<InputFile> &files,
                                            std::vector<std::uint8_t> &groups) {
    Pending p;
    p.gen.has_seed = h.seed();
    p.gen.mapping_revision = h.revision == 5 ? solid_seed_revision : seed_revision;
    p.gen.plan = plan;
    p.gen.grouping = initial_grouping(plan, files);
    p.gen.groups.resize(p.gen.grouping.groups());
    p.new_dicts.resize(p.gen.groups.size());
    groups.clear();
    for (const auto &f : files) {
        groups.push_back(group_of(p.gen, f.path, f.data.size()));
    }
    if (!h.solid()) {
        for (std::size_t g = 0; g < p.gen.groups.size(); ++g) {
            std::vector<const InputFile *> members;
            for (std::size_t i = 0; i < files.size(); ++i) {
                if (groups[i] == g) {
                    members.push_back(&files[i]);
                }
            }
            if (dict_sizes[plan.d[g]] != 0 && !members.empty()) {
                auto d = train(members, plan.d[g], plan.t[g]);
                if (!d) {
                    return std::unexpected(d.error());
                }
                p.new_dicts[g] = std::move(*d);
            }
        }
    }
    return p;
}

std::vector<std::span<const std::byte>> dict_views(const Pending &p,
                                                   std::span<const std::byte> old) {
    std::vector<std::span<const std::byte>> views;
    for (std::size_t g = 0; g < p.gen.groups.size(); ++g) {
        if (!p.new_dicts[g].empty()) {
            views.emplace_back(p.new_dicts[g]);
        } else {
            const auto &grp = p.gen.groups[g];
            views.push_back(old.subspan(static_cast<std::size_t>(grp.dict_offset), grp.dict_size));
        }
    }
    return views;
}

std::expected<std::vector<std::byte>, Error> lay_out_fresh(const Header &h, Pending p) {
    Model m;
    m.header = h;
    auto out = header_of(h);
    if (auto r = layout(out, m, std::move(p)); !r) {
        return std::unexpected(r.error());
    }
    finish_container(out, m);
    return out;
}

Header header_for(const std::string &mode, std::uint32_t redundancy, std::uint32_t cpb) {
    Header h;
    h.revision = (mode == "seed-solid" || mode == "explicit-solid") ? 5 : version;
    h.flags = mode == "seed-solid"       ? std::uint32_t{flag_seed | flag_solid}
              : mode == "explicit-solid" ? std::uint32_t{flag_solid}
              : mode == "seed"           ? std::uint32_t{flag_seed}
              : mode == "solid"          ? std::uint32_t{flag_solid}
                                         : 0U;
    h.redundancy = redundancy;
    h.checkpoint_bytes = redundancy == red_checkpoint ? cpb : 0;
    return h;
}

struct Packed {
    std::vector<std::byte> bytes;
    std::uint32_t redundancy = red_off;
    bool recommended = false;
    std::array<std::uint64_t, 3> candidate_bytes{};
};
// Packs once per redundancy level from the same encoded data (encoding is the expensive part),
// then keeps the requested level, or the one rule rec-r0 recommends.
std::expected<Packed, Error> pack_bytes(const std::string &mode, const Plan &plan,
                                        const std::vector<InputFile> &files,
                                        std::optional<std::uint32_t> wanted, std::uint32_t cpb,
                                        std::uint64_t frame_limit = research::max_solid_bytes,
                                        const std::optional<RecipeFields> &recipe = std::nullopt) {
    std::vector<std::uint8_t> groups;
    const auto base = header_for(mode, red_off, cpb);
    if (recipe && (base.revision != 5 || frame_limit != recipe_frame_limit(*recipe))) {
        return std::unexpected(bad("a recipe needs a solid strategy and its own frame limit"));
    }
    if (base.revision == 5) {
        if (!valid_solid_plan(plan) || (wanted && *wanted != red_off)) {
            return std::unexpected(
                bad("solid strategy requires a canonical solid plan and off redundancy"));
        }
        wanted = red_off;
    }
    auto p = fresh_pending(base, plan, files, groups);
    if (!p) {
        return std::unexpected(p.error());
    }
    if (recipe) {
        p->gen.mapping_revision = solid_recipe_revision;
        p->gen.recipe = recipe;
    }
    if (auto r = encode_pending(*p, base, files, groups, dict_views(*p, {}), frame_limit); !r) {
        return std::unexpected(r.error());
    }
    Packed out;
    if (wanted) {
        // A single explicit layout does not need a second full copy of encoded payloads.
        auto bytes = lay_out_fresh(header_for(mode, *wanted, cpb), std::move(*p));
        if (!bytes) {
            return std::unexpected(bytes.error());
        }
        out.redundancy = *wanted;
        out.candidate_bytes[*wanted] = bytes->size();
        out.bytes = std::move(*bytes);
        return out;
    }
    std::array<std::vector<std::byte>, 3> all;
    for (std::uint32_t red = red_off; red <= red_file; ++red) {
        auto bytes = lay_out_fresh(header_for(mode, red, cpb), *p);
        if (!bytes) {
            return std::unexpected(bytes.error());
        }
        out.candidate_bytes[red] = bytes->size();
        all[red] = std::move(*bytes);
    }
    out.recommended = true;
    const auto off = static_cast<double>(out.candidate_bytes[red_off]);
    const auto within = [&](std::uint32_t red) {
        return static_cast<double>(out.candidate_bytes[red]) - off <= rec_overhead_budget * off;
    };
    out.redundancy = out.candidate_bytes[red_off] < rec_small_archive ? red_off
                     : within(red_file)                               ? red_file
                     : within(red_checkpoint)                         ? red_checkpoint
                                                                      : red_off;
    out.bytes = std::move(all[out.redundancy]);
    return out;
}

// The D(b) heuristic of explicit and solid mode: the group of the existing file, among those
// that share `ref`'s grouping, with the longest common path prefix.
std::uint8_t nearest_group(const Model &m, const Gen &ref, std::string_view path) {
    std::size_t best_len = 0;
    std::uint8_t best = ref.grouping.other;
    for (const auto &e : m.entries) {
        const auto &g = m.gens[e.gen];
        if (g.groups != ref.groups || g.grouping != ref.grouping) {
            continue;
        }
        std::size_t n = 0;
        while (n < e.path.size() && n < path.size() && e.path[n] == path[n]) {
            ++n;
        }
        if (n > best_len || (n == best_len && n > 0 && e.group < best)) {
            best_len = n;
            best = e.group;
        }
    }
    return best;
}

std::expected<std::vector<std::byte>, Error>
append_bytes(std::span<const std::byte> old, const std::vector<InputFile> &added,
             const std::string &how, const std::optional<Plan> &plan,
             std::uint64_t frame_limit = research::max_solid_bytes,
             const std::optional<RecipeFields> &recipe = std::nullopt) {
    auto parsed = parse(old);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    Model m = std::move(*parsed);
    const auto &h = m.header;
    // With a recipe every generation has its own frame limit, so old frames are not bound by
    // the new one; without, the old bounded-append rule (old frames within the limit) holds.
    const bool recipe_append = recipe || (!plan && m.gens.back().recipe);
    if (recipe_append) {
        const auto fields = recipe ? *recipe : *m.gens.back().recipe;
        if (h.revision != 5 || how != "segment" || frame_limit != recipe_frame_limit(fields) ||
            (recipe && !plan)) {
            return std::unexpected(bad("recipe append needs a solid segment and its frame limit"));
        }
    } else if (frame_limit == 0 || frame_limit > research::max_solid_bytes ||
               std::ranges::any_of(m.frames,
                                   [frame_limit](const Frame &f) { return f.raw > frame_limit; })) {
        return std::unexpected(bad("old archive exceeds the requested solid frame limit"));
    }
    if (h.revision == 5 && plan && !valid_solid_plan(*plan)) {
        return std::unexpected(bad("invalid solid generation plan"));
    }
    std::set<std::string> existing;
    for (const auto &e : m.entries) {
        existing.insert(e.path);
    }
    for (const auto &f : added) {
        if (existing.contains(f.path)) {
            return std::unexpected(
                Error{ErrorCode::output_conflict, "appended path already exists: " + f.path});
        }
    }
    if (added.empty()) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "nothing to append"});
    }
    const Gen last = m.gens.back();
    if (how == "regroup") {
        // Solid baseline: re-compress everything as one generation with the original grouping
        // (needs a single grouping across generations). A fresh container, not an extension.
        if (!h.solid() || plan) {
            return std::unexpected(
                Error{ErrorCode::invalid_argument, "regroup is for solid containers, no plan"});
        }
        for (const auto &g : m.gens) {
            if (g.grouping != m.gens[0].grouping || (h.revision == 5 && g.plan != m.gens[0].plan)) {
                return std::unexpected(
                    Error{ErrorCode::invalid_argument, "regroup needs one grouping"});
            }
        }
        auto decoded = decode_all(m, old);
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        std::vector<InputFile> all;
        std::vector<std::uint8_t> groups;
        for (const auto &e : m.entries) {
            const auto it = decoded->find(e.path);
            if (it == decoded->end()) {
                return std::unexpected(bad("regroup file missing from decoded input"));
            }
            all.push_back({e.path, std::move(it->second)});
            groups.push_back(e.group);
        }
        for (const auto &f : added) {
            groups.push_back(h.revision == 5 ? group_of(last, f.path, f.data.size())
                                             : nearest_group(m, last, f.path));
            all.push_back(f);
        }
        decoded->clear();
        Pending p;
        p.gen = m.gens[0];
        p.new_dicts.resize(p.gen.groups.size());
        if (auto r = encode_pending(p, h, all, groups, dict_views(p, old), frame_limit); !r) {
            return std::unexpected(r.error());
        }
        return lay_out_fresh(h, std::move(p));
    }
    std::vector<std::uint8_t> groups;
    Pending p;
    if (plan) {
        // A new generation with its own Seed (queue variant B): grouping and dictionaries from
        // the new files only; old data is untouched.
        auto fresh = fresh_pending(h, *plan, added, groups);
        if (!fresh) {
            return std::unexpected(fresh.error());
        }
        p = std::move(*fresh);
        if (recipe) {
            p.gen.mapping_revision = solid_recipe_revision;
            p.gen.recipe = recipe;
        }
    } else {
        // Variant A: keep the previous generation's Seed, key table and dictionaries.
        p.gen = last;
        p.new_dicts.resize(p.gen.groups.size());
        for (const auto &f : added) {
            groups.push_back((h.seed() || h.revision == 5) ? group_of(last, f.path, f.data.size())
                                                           : nearest_group(m, last, f.path));
        }
    }
    if (auto r = encode_pending(p, h, added, groups, dict_views(p, old), frame_limit); !r) {
        return std::unexpected(r.error());
    }
    std::vector<std::byte> out(old.begin(), old.end());
    if (auto r = layout(out, m, std::move(p)); !r) {
        return std::unexpected(r.error());
    }
    finish_container(out, m);
    return out;
}

// ---- reporting ----------------------------------------------------------------------------------
std::string parts_json(const Model &m, std::span<const std::byte> bytes) {
    std::uint64_t dicts = 0, data = 0, gen_records = 0, key_tables = 0;
    for (std::size_t i = 0; i < m.gens.size(); ++i) {
        const auto &g = m.gens[i];
        const auto record_end = g.record_offset + record_overhead + g.body.size();
        gen_records += record_overhead + g.body.size();
        for (const auto &grp : g.groups) {
            dicts += grp.dict_offset >= record_end ? grp.dict_size : 0;
        }
        key_tables += 4;
        for (const auto &key : g.grouping.keys | std::views::keys) {
            key_tables += 2 + key.size() + 1;
        }
    }
    for (const auto &e : m.entries) {
        for (const auto &b : e.blocks) {
            data += b.stored;
        }
    }
    for (const auto &f : m.frames) {
        data += f.stored;
    }
    const auto redundancy =
        m.record_bytes[rec_file] + m.record_bytes[rec_checkpoint] + m.record_bytes[rec_frame];
    const auto trailer = bytes.size() - footer_bytes - m.trailer_offset;
    const auto stale = m.trailer_offset - header_bytes - gen_records - dicts - data - redundancy;
    // Group metadata (H-meta): key tables plus the Seed records, or per-file group bytes (in the
    // trailer, and uncompressed in FIL records when the level is "file").
    const auto per_file = m.header.explicit_groups()
                              ? m.entries.size() * (m.header.redundancy == red_file ? 2U : 1U)
                              : 0U;
    const auto group_meta = key_tables +
                            ((m.header.seed() || m.header.revision == 5) ? 8 * m.gens.size() : 0) +
                            per_file;
    return "\"container_revision\":" + std::to_string(m.header.revision) +
           ",\"strategy_revision\":" +
           std::to_string(m.header.revision == 5 ? solid_seed_revision : seed_revision) +
           ",\"container_bytes\":" + std::to_string(bytes.size()) + ",\"redundancy\":\"" +
           redundancy_names[m.header.redundancy] +
           "\",\"checkpoint_bytes\":" + std::to_string(m.header.checkpoint_bytes) +
           ",\"parts\":{\"header\":" + std::to_string(header_bytes) +
           ",\"generation_records\":" + std::to_string(gen_records) +
           ",\"dictionaries\":" + std::to_string(dicts) + ",\"data\":" + std::to_string(data) +
           ",\"redundancy_records\":" + std::to_string(redundancy) +
           ",\"stale_trailers\":" + std::to_string(stale) +
           ",\"trailer\":" + std::to_string(trailer) +
           ",\"footer\":" + std::to_string(footer_bytes) +
           ",\"group_metadata\":" + std::to_string(group_meta) +
           "},\"files\":" + std::to_string(m.entries.size()) +
           ",\"generations\":" + std::to_string(m.gens.size());
}

std::optional<Plan> plan_arg(std::string_view hex) {
    const auto seed = hex32(hex);
    const auto plan = seed ? decode_seed(*seed) : std::nullopt;
    if (!plan || canonical(*plan) != *plan) {
        return std::nullopt;
    }
    return plan;
}

std::uint64_t input_bytes_of(const std::vector<InputFile> &files) {
    std::uint64_t n = 0;
    for (const auto &f : files) {
        n += f.data.size();
    }
    return n;
}

// ---- volumes (seed-append-r1 7.6) ---------------------------------------------------------------
// A volume set cuts the logical v4 byte stream into files vol-NNNNN.sadv. Each volume:
//   fixed header (60) | carry | CRC32C(fixed header + carry) (4) | data
// fixed header: magic, version, volume index, archive id, logical start, data length, nominal
// volume size, CRC32C of the whole previous volume file (0 for volume 0), carry mode, carry
// length. The carry repeats logical byte ranges from before the volume (the GEN record of the
// generation in effect, and with "dicts" its dictionaries) so the volume can be used when the
// earlier volumes are missing. Records keep their logical offsets, so nothing else changes.
// An append writes new volumes only; existing volume files are never opened for writing.
constexpr Tag volume_magic{'S', 'A', 'D', 'X', 'V', 'O', 'L', '4'};
constexpr std::uint32_t volume_version = 1;
constexpr std::size_t volume_fixed = 60;
constexpr std::uint64_t min_volume_bytes = 4096;
constexpr std::uint32_t max_volumes = 10000;
constexpr std::uint64_t max_logical = 2ULL << 30;
constexpr std::size_t max_carry = std::size_t{1} << 24;
enum : std::uint32_t { carry_none = 0, carry_gen = 1, carry_dicts = 2 };
constexpr std::array<const char *, 3> carry_names{"none", "gen", "dicts"};

// Deterministic archive identity: FNV-1a 64 over the header, the GEN record of generation 0 and
// its dictionaries (bytes an append never changes). Distinguishes unrelated archives; not a
// security property.
std::uint64_t archive_id(std::span<const std::byte> logical, const Model &m) {
    std::uint64_t h = 1469598103934665603ULL;
    const auto n = std::min<std::uint64_t>(logical.size(), m.data_start.front());
    for (const auto b : logical.first(static_cast<std::size_t>(n))) {
        h = (h ^ std::to_integer<std::uint64_t>(b)) * 1099511628211ULL;
    }
    return h;
}
std::string volume_name(std::uint32_t index) {
    auto digits = std::to_string(index);
    return "vol-" + std::string(5 - std::min<std::size_t>(5, digits.size()), '0') + digits +
           ".sadv";
}
std::optional<std::uint32_t> volume_index_of(std::string_view name) {
    if (name.size() != 14 || !name.starts_with("vol-") || !name.ends_with(".sadv")) {
        return std::nullopt;
    }
    const auto n = number(name.substr(4, 5));
    return n && *n < max_volumes ? std::optional{static_cast<std::uint32_t>(*n)} : std::nullopt;
}

struct Volume {
    std::uint32_t index = 0;
    std::uint64_t id = 0, start = 0, nominal = 0;
    std::uint32_t prev_crc = 0, carry_mode = carry_none;
    std::vector<std::pair<std::uint64_t, std::vector<std::byte>>> carry;
    std::vector<std::byte> data;
    std::uint32_t file_crc = 0; // CRC32C of the whole volume file
    std::uint64_t end() const { return start + data.size(); }
};

std::vector<std::byte> volume_file(const Volume &v) {
    std::vector<std::byte> carry;
    put(carry, v.carry.size(), 4);
    for (const auto &[off, bytes] : v.carry) {
        put(carry, off, 8);
        put(carry, bytes.size(), 4);
        carry.insert(carry.end(), bytes.begin(), bytes.end());
    }
    std::vector<std::byte> out(volume_magic.size());
    std::memcpy(out.data(), volume_magic.data(), volume_magic.size());
    put(out, volume_version, 4);
    put(out, v.index, 4);
    put(out, v.id, 8);
    put(out, v.start, 8);
    put(out, v.data.size(), 8);
    put(out, v.nominal, 8);
    put(out, v.prev_crc, 4);
    put(out, v.carry_mode, 4);
    put(out, carry.size(), 4);
    out.insert(out.end(), carry.begin(), carry.end());
    put(out, seedarc::experimental::crc32c(out), 4);
    out.insert(out.end(), v.data.begin(), v.data.end());
    return out;
}

std::expected<Volume, Error> parse_volume(std::span<const std::byte> file) {
    const auto vbad = [](const char *what) {
        return std::unexpected(
            Error{ErrorCode::integrity_error, std::string{"research volume v4: "} + what});
    };
    if (file.size() < volume_fixed + 8 ||
        std::memcmp(file.data(), volume_magic.data(), volume_magic.size()) != 0) {
        return vbad("bad magic");
    }
    Reader r{file, volume_magic.size()};
    const auto ver = r.get(4), index = r.get(4), id = r.get(8), start = r.get(8), len = r.get(8),
               nominal = r.get(8), prev = r.get(4), mode = r.get(4), carry_len = r.get(4);
    if (!carry_len || *ver != volume_version || *index >= max_volumes || *mode > carry_dicts ||
        *carry_len > max_carry || *nominal < min_volume_bytes || *nominal > max_logical ||
        *len == 0 || *len > *nominal || *start > max_logical || max_logical - *start < *len ||
        file.size() - volume_fixed - 4 < *carry_len ||
        file.size() - volume_fixed - 4 - *carry_len != *len) {
        return vbad("bad header");
    }
    const auto head = file.first(volume_fixed + static_cast<std::size_t>(*carry_len));
    Reader c{file, head.size()};
    if (c.get(4) != seedarc::experimental::crc32c(head)) {
        return vbad("header checksum mismatch");
    }
    Volume v;
    v.index = static_cast<std::uint32_t>(*index);
    v.id = *id;
    v.start = *start;
    v.nominal = *nominal;
    v.prev_crc = static_cast<std::uint32_t>(*prev);
    v.carry_mode = static_cast<std::uint32_t>(*mode);
    Reader k{head, volume_fixed};
    const auto count = k.get(4);
    if (!count || *count > 64) {
        return vbad("bad carry");
    }
    for (std::uint64_t i = 0; i < *count; ++i) {
        const auto off = k.get(8), n = k.get(4);
        if (!n || *n == 0 || head.size() - k.at < *n || *off >= *start || *off > max_logical) {
            return vbad("bad carry range");
        }
        v.carry.emplace_back(
            *off, std::vector<std::byte>{head.begin() + static_cast<std::ptrdiff_t>(k.at),
                                         head.begin() + static_cast<std::ptrdiff_t>(k.at + *n)});
        k.at += static_cast<std::size_t>(*n);
    }
    if (k.at != head.size() || (v.carry_mode == carry_none && !v.carry.empty())) {
        return vbad("bad carry");
    }
    const auto data = file.subspan(head.size() + 4);
    v.data.assign(data.begin(), data.end());
    v.file_crc = seedarc::experimental::crc32c(file);
    return v;
}

// The logical ranges a volume starting at `s` carries: the container header, the GEN record of
// the last generation that starts before `s`, plus (mode "dicts") every dictionary it uses.
std::vector<std::pair<std::uint64_t, std::uint64_t>> carry_ranges(const Model &m, std::uint64_t s,
                                                                  std::uint32_t mode) {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    const Gen *in_effect = nullptr;
    for (const auto &g : m.gens) {
        if (g.record_offset < s) {
            in_effect = &g;
        }
    }
    if (mode == carry_none || in_effect == nullptr) {
        return ranges;
    }
    ranges.emplace_back(0, header_bytes); // the container header (flags, redundancy level)
    ranges.emplace_back(in_effect->record_offset,
                        in_effect->record_offset + record_overhead + in_effect->body.size());
    if (mode == carry_dicts) {
        for (const auto &grp : in_effect->groups) {
            const auto range = std::pair{grp.dict_offset, grp.dict_offset + grp.dict_size};
            if (grp.dict_size != 0 && std::ranges::find(ranges, range) == ranges.end()) {
                ranges.push_back(range);
            }
        }
    }
    std::erase_if(ranges, [&](const auto &r) { return r.first >= s; });
    return ranges;
}

// Volumes covering logical bytes [from, end) of `logical`, numbered from `first_index`.
std::expected<std::vector<Volume>, Error> make_volumes(std::span<const std::byte> logical,
                                                       const Model &m, std::uint64_t from,
                                                       std::uint32_t first_index,
                                                       std::uint64_t nominal, std::uint32_t mode,
                                                       std::uint32_t prev_crc) {
    if (m.header.revision == 5) {
        return std::unexpected(bad("volumes are not implemented for solid strategy revision 5"));
    }
    if (logical.size() > max_logical) {
        return std::unexpected(Error{ErrorCode::resource_limit, "archive too large for volumes"});
    }
    const auto id = archive_id(logical, m);
    std::vector<Volume> out;
    auto index = first_index;
    for (auto s = from; s < logical.size(); s += nominal, ++index) {
        if (index >= max_volumes) {
            return std::unexpected(Error{ErrorCode::resource_limit, "too many volumes"});
        }
        Volume v;
        v.index = index;
        v.id = id;
        v.start = s;
        v.nominal = nominal;
        v.prev_crc = prev_crc;
        v.carry_mode = mode;
        const auto len = std::min<std::uint64_t>(nominal, logical.size() - s);
        const auto data =
            logical.subspan(static_cast<std::size_t>(s), static_cast<std::size_t>(len));
        v.data.assign(data.begin(), data.end());
        for (const auto &[b, e] : carry_ranges(m, s, mode)) {
            const auto r =
                logical.subspan(static_cast<std::size_t>(b), static_cast<std::size_t>(e - b));
            v.carry.emplace_back(b, std::vector<std::byte>{r.begin(), r.end()});
        }
        auto file = volume_file(v);
        prev_crc = seedarc::experimental::crc32c(file);
        v.file_crc = prev_crc;
        out.push_back(std::move(v));
    }
    return out;
}

// A volume directory as found: present volumes by index, checked for consistency where
// neighbors are both present (same archive, chained CRCs, contiguous logical ranges).
struct VolumeSet {
    std::map<std::uint32_t, Volume> volumes;
    std::uint64_t id = 0, nominal = 0;
    std::uint32_t carry_mode = carry_none;
};
std::expected<VolumeSet, Error> read_volumes(const fs::path &dir,
                                             std::optional<std::uint32_t> skip = std::nullopt) {
    VolumeSet set;
    std::error_code ec;
    std::uint64_t total = 0;
    for (fs::directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
        if (ec) {
            return std::unexpected(Error{ErrorCode::io_error, "cannot list the volume directory"});
        }
        const auto index = volume_index_of(utf8(it->path().filename()));
        if (!index || it->is_symlink() || !it->is_regular_file() || index == skip) {
            continue;
        }
        auto file = read_file(it->path());
        if (!file) {
            return std::unexpected(file.error());
        }
        total += file->size();
        if (total > max_logical + max_volumes * (max_carry + volume_fixed)) {
            return std::unexpected(Error{ErrorCode::resource_limit, "volumes too large"});
        }
        auto v = parse_volume(*file);
        if (!v) {
            return std::unexpected(v.error());
        }
        if (v->index != *index) {
            return std::unexpected(bad("volume number differs from its file name"));
        }
        set.volumes.emplace(*index, std::move(*v));
    }
    if (set.volumes.empty()) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "no volumes found"});
    }
    const auto &any = set.volumes.begin()->second;
    set.id = any.id;
    set.nominal = any.nominal;
    set.carry_mode = any.carry_mode;
    const Volume *prev = nullptr;
    for (const auto &[index, v] : set.volumes) {
        if (v.id != set.id || v.nominal != set.nominal || v.carry_mode != set.carry_mode) {
            return std::unexpected(bad("volumes belong to different archives or settings"));
        }
        if (index == 0 && (v.start != 0 || v.prev_crc != 0)) {
            return std::unexpected(bad("bad first volume"));
        }
        if (prev != nullptr &&
            (v.start < prev->end() || (prev->index + 1 == index &&
                                       (v.start != prev->end() || v.prev_crc != prev->file_crc)))) {
            return std::unexpected(bad("volumes out of order or not chained"));
        }
        prev = &v;
    }
    return set;
}
bool complete(const VolumeSet &set) {
    return set.volumes.size() == set.volumes.rbegin()->first + 1;
}
// The logical stream with what is present (volume data and carried ranges); holes are zero.
std::expected<std::pair<std::vector<std::byte>, Presence>, Error> assemble(const VolumeSet &set) {
    std::uint64_t size = 0;
    for (const auto &v : set.volumes | std::views::values) {
        size = std::max(size, v.end());
    }
    if (size > max_logical) {
        return std::unexpected(Error{ErrorCode::resource_limit, "archive too large"});
    }
    std::vector<std::byte> logical(static_cast<std::size_t>(size));
    Presence present;
    for (const auto &v : set.volumes | std::views::values) {
        std::ranges::copy(v.data, logical.begin() + static_cast<std::ptrdiff_t>(v.start));
        present.add(v.start, v.end());
    }
    for (const auto &v : set.volumes | std::views::values) {
        for (const auto &[off, bytes] : v.carry) {
            if (off > size || size - off < bytes.size()) {
                continue; // refers past what we have: of no use here
            }
            // Where the range is already present, the copy must agree; then fill the rest.
            const auto end = off + bytes.size();
            for (const auto &[b, e] : present.runs) {
                const auto lo = std::max(b, off), hi = std::min(e, end);
                if (lo < hi && !std::equal(bytes.begin() + static_cast<std::ptrdiff_t>(lo - off),
                                           bytes.begin() + static_cast<std::ptrdiff_t>(hi - off),
                                           logical.begin() + static_cast<std::ptrdiff_t>(lo))) {
                    return std::unexpected(bad("carried copy differs from the volume data"));
                }
            }
            std::ranges::copy(bytes, logical.begin() + static_cast<std::ptrdiff_t>(off));
            present.add(off, end);
        }
    }
    return std::pair{std::move(logical), std::move(present)};
}
std::expected<std::vector<std::byte>, Error> join(const VolumeSet &set) {
    if (!complete(set)) {
        return std::unexpected(Error{ErrorCode::integrity_error, "volume set is incomplete"});
    }
    auto a = assemble(set);
    if (!a) {
        return std::unexpected(a.error());
    }
    // The joined stream must be a valid container whose identity is the one in the volumes.
    auto m = parse(a->first);
    if (!m) {
        return std::unexpected(m.error());
    }
    if (archive_id(a->first, *m) != set.id) {
        return std::unexpected(bad("volumes do not match their archive id"));
    }
    return std::move(a->first);
}
std::expected<void, Error> write_volumes(const fs::path &dir, const std::vector<Volume> &vols) {
    for (const auto &v : vols) {
        if (fs::exists(dir / volume_name(v.index))) {
            return std::unexpected(Error{ErrorCode::output_conflict, "volume exists"});
        }
    }
    for (const auto &v : vols) {
        if (auto r = write_new(dir / volume_name(v.index), volume_file(v)); !r) {
            return std::unexpected(r.error());
        }
    }
    return {};
}

// ---- plan search: cold and warm start (seed-append-r1 7.5.1) ------------------------------------
// The G5 r1 search algorithms ("seed": random single-field hill climbing with a stall limit;
// "coord": coordinate descent over every value of every active field), re-implemented on this
// tool's insertion-stable grouping so the plan found is the plan packed. The budget is counted as
// in G5: input bytes read by training and trial encoding, cache hits free. The objective is the
// part of the container a plan changes: dictionaries + data + key table + Seed record (per-file
// metadata is the same for every plan and left out). "restart" re-starts from a random plan when
// stuck (G5 behavior, a cold search); "local" stops instead (a warm search that stays near its
// start). The start plan is always the first candidate, so the result is never worse than it.
Plan default_start() { // G-dict-r1, the G5 start plan: one group, 64 KiB dictionary
    Plan p;
    p.d[0] = 2;
    return canonical(p);
}
struct Field {
    int id;
    int values;
};
std::vector<Field> active_fields(const Plan &p) {
    std::vector<Field> f{{0, 4}, {1, static_cast<int>(max_groups)}};
    for (int i = 0; i < p.k; ++i) {
        f.push_back({2 + 2 * i, 4});
        f.push_back({3 + 2 * i, 2});
    }
    return f;
}
int field_value(const Plan &p, int id) {
    if (id == 0) {
        return p.rule;
    }
    if (id == 1) {
        return p.k - 1;
    }
    const auto g = static_cast<std::size_t>((id - 2) / 2);
    return id % 2 == 0 ? p.d[g] : p.t[g];
}
Plan with_field(Plan p, int id, int value) {
    if (id == 0) {
        p.rule = static_cast<std::uint8_t>(value);
    } else if (id == 1) {
        p.k = static_cast<std::uint8_t>(value + 1);
    } else {
        const auto g = static_cast<std::size_t>((id - 2) / 2);
        (id % 2 == 0 ? p.d[g] : p.t[g]) = static_cast<std::uint8_t>(value);
    }
    return canonical(p);
}
struct Rng {
    std::uint64_t state;
    std::uint64_t next() {
        const auto out = seedarc::third_party::splitmix64_step(state);
        state += 0x9e3779b97f4a7c15ULL;
        return out;
    }
    std::uint64_t below(std::uint64_t n) { // uniform in [0, n) without modulo bias
        const auto limit = UINT64_MAX - UINT64_MAX % n;
        for (;;) {
            if (const auto v = next(); v < limit) {
                return v % n;
            }
        }
    }
};
Plan random_plan(Rng &rng) {
    Plan p;
    p.rule = static_cast<std::uint8_t>(rng.below(4));
    p.k = static_cast<std::uint8_t>(rng.below(max_groups) + 1);
    for (std::size_t i = 0; i < max_groups; ++i) {
        p.d[i] = static_cast<std::uint8_t>(rng.below(4));
        p.t[i] = static_cast<std::uint8_t>(rng.below(2));
    }
    return canonical(p);
}
std::uint64_t key_table_bytes(const Grouping &g) {
    std::uint64_t n = 4;
    for (const auto &key : g.keys | std::views::keys) {
        n += 2 + key.size() + 1;
    }
    return n;
}

class PlanEvaluator {
  public:
    PlanEvaluator(const std::vector<InputFile> &files, std::uint64_t budget)
        : files_(files), budget_(budget) {}
    // nullopt: the uncached part would exceed the remaining budget (nothing is spent).
    std::expected<std::optional<std::uint64_t>, Error> evaluate(const Plan &plan) {
        if (const auto it = plans_.find(plan); it != plans_.end()) {
            ++plan_cache_hits_;
            return std::optional{it->second};
        }
        const auto grouping = initial_grouping(plan, files_);
        std::vector<std::vector<std::uint32_t>> members(grouping.groups());
        for (std::size_t i = 0; i < files_.size(); ++i) {
            members[grouping.of(plan.rule, files_[i].path, files_[i].data.size())].push_back(
                static_cast<std::uint32_t>(i));
        }
        std::uint64_t cost = 0;
        for (std::size_t g = 0; g < members.size(); ++g) {
            if (!members[g].empty() && !groups_.contains({members[g], plan.d[g], plan.t[g]})) {
                cost += group_cost(members[g], plan.d[g], plan.t[g]);
            }
        }
        if (spent_ + cost > budget_) {
            return std::optional<std::uint64_t>{};
        }
        std::uint64_t bytes = key_table_bytes(grouping) + 8;
        for (std::size_t g = 0; g < members.size(); ++g) {
            if (members[g].empty()) {
                continue;
            }
            const Key key{members[g], plan.d[g], plan.t[g]};
            auto it = groups_.find(key);
            if (it == groups_.end()) {
                auto n = encode_group(members[g], plan.d[g], plan.t[g]);
                if (!n) {
                    return std::unexpected(n.error());
                }
                ++group_evaluations_;
                it = groups_.emplace(key, *n).first;
            } else {
                ++group_cache_hits_;
            }
            bytes += it->second;
        }
        spent_ += cost;
        ++plan_evaluations_;
        plans_.emplace(plan, bytes);
        return std::optional{bytes};
    }
    std::uint64_t spent() const { return spent_; }
    std::string stats_json() const {
        return "\"plan_evaluations\":" + std::to_string(plan_evaluations_) +
               ",\"plan_cache_hits\":" + std::to_string(plan_cache_hits_) +
               ",\"group_evaluations\":" + std::to_string(group_evaluations_) +
               ",\"group_cache_hits\":" + std::to_string(group_cache_hits_) +
               ",\"budget_bytes\":" + std::to_string(budget_) +
               ",\"budget_spent_bytes\":" + std::to_string(spent_);
    }

  private:
    using Key = std::tuple<std::vector<std::uint32_t>, std::uint8_t, std::uint8_t>;
    std::uint64_t group_cost(const std::vector<std::uint32_t> &members, std::uint8_t d,
                             std::uint8_t t) const {
        std::uint64_t cost = 0;
        for (const auto m : members) {
            cost += files_[m].data.size();
            if (dict_sizes[d] != 0) {
                cost += std::min<std::uint64_t>(files_[m].data.size(), train_prefix[t]);
            }
        }
        return cost;
    }
    // Dictionary plus data bytes of one group (exactly what packing writes for it).
    std::expected<std::uint64_t, Error> encode_group(const std::vector<std::uint32_t> &members,
                                                     std::uint8_t d, std::uint8_t t) const {
        std::vector<const InputFile *> ptrs;
        for (const auto m : members) {
            ptrs.push_back(&files_[m]);
        }
        std::vector<std::byte> dict;
        if (dict_sizes[d] != 0) {
            auto trained = train(ptrs, d, t);
            if (!trained) {
                return std::unexpected(trained.error());
            }
            dict = std::move(*trained);
        }
        std::uint64_t bytes = dict.size();
        std::vector<std::byte> scratch;
        for (const auto *f : ptrs) {
            scratch.clear();
            auto blocks = encode_blocks(*f, dict, scratch);
            if (!blocks) {
                return std::unexpected(blocks.error());
            }
            bytes += scratch.size();
        }
        return bytes;
    }
    const std::vector<InputFile> &files_;
    std::uint64_t budget_;
    std::uint64_t spent_ = 0;
    std::map<Plan, std::uint64_t> plans_;
    std::map<Key, std::uint64_t> groups_;
    std::uint64_t plan_evaluations_ = 0, plan_cache_hits_ = 0;
    std::uint64_t group_evaluations_ = 0, group_cache_hits_ = 0;
};

struct SearchState {
    Plan best;
    std::uint64_t best_bytes = UINT64_MAX;
    std::uint64_t start_bytes = 0;
    std::uint64_t proposals = 0, restarts = 0;
    std::string trajectory; // [proposal, spent, best_bytes] at each improvement
    std::string stop = "budget";
    bool done = false;
};
constexpr std::uint64_t max_proposals = 20000;

std::expected<std::optional<std::uint64_t>, Error> try_plan(PlanEvaluator &ev, SearchState &s,
                                                            const Plan &p) {
    if (s.proposals >= max_proposals) {
        s.done = true;
        s.stop = "proposal_limit";
        return std::optional<std::uint64_t>{};
    }
    ++s.proposals;
    auto r = ev.evaluate(p);
    if (!r) {
        return std::unexpected(r.error());
    }
    if (!*r) {
        s.done = true;
        s.stop = "budget";
        return std::optional<std::uint64_t>{};
    }
    if (**r < s.best_bytes) {
        s.best_bytes = **r;
        s.best = p;
        s.trajectory += (s.trajectory.empty() ? "[" : ",[") + std::to_string(s.proposals) + "," +
                        std::to_string(ev.spent()) + "," + std::to_string(**r) + "]";
    }
    return r;
}

// Re-starts from a random plan (restart mode) or ends the search (local mode). Returns the new
// current size, or nullopt when the search is over.
std::expected<std::optional<std::uint64_t>, Error>
stuck(PlanEvaluator &ev, SearchState &s, Rng &rng, bool restart, Plan &current, const char *why) {
    if (!restart) {
        s.done = true;
        s.stop = why;
        return std::optional<std::uint64_t>{};
    }
    ++s.restarts;
    current = random_plan(rng);
    return try_plan(ev, s, current);
}

std::expected<void, Error> search_seed(PlanEvaluator &ev, SearchState &s, Rng &rng, Plan current,
                                       int stall, bool restart) {
    auto first = try_plan(ev, s, current);
    if (!first || !*first) {
        return first ? std::expected<void, Error>{} : std::unexpected(first.error());
    }
    s.start_bytes = **first;
    std::uint64_t current_bytes = **first;
    int without = 0;
    while (!s.done) {
        const auto fields = active_fields(current);
        const auto field = fields[rng.below(fields.size())];
        auto v = static_cast<int>(rng.below(static_cast<std::uint64_t>(field.values - 1)));
        if (v >= field_value(current, field.id)) {
            ++v; // a different value
        }
        const auto candidate = with_field(current, field.id, v);
        if (candidate != current) {
            auto r = try_plan(ev, s, candidate);
            if (!r) {
                return std::unexpected(r.error());
            }
            if (!*r) {
                return {};
            }
            if (**r < current_bytes) {
                current = candidate;
                current_bytes = **r;
                without = 0;
                continue;
            }
        }
        if (++without >= stall) {
            without = 0;
            auto r = stuck(ev, s, rng, restart, current, "stalled");
            if (!r) {
                return std::unexpected(r.error());
            }
            if (!*r) {
                return {};
            }
            current_bytes = **r;
        }
    }
    return {};
}

std::expected<void, Error> search_coord(PlanEvaluator &ev, SearchState &s, Rng &rng, Plan current,
                                        bool restart) {
    auto first = try_plan(ev, s, current);
    if (!first || !*first) {
        return first ? std::expected<void, Error>{} : std::unexpected(first.error());
    }
    s.start_bytes = **first;
    std::uint64_t current_bytes = **first;
    while (!s.done) {
        auto fields = active_fields(current);
        for (std::size_t i = fields.size(); i > 1; --i) {
            std::swap(fields[i - 1], fields[rng.below(i)]);
        }
        bool improved = false;
        for (const auto &field : fields) {
            if (field.id >= 2 + 2 * current.k) {
                continue; // k shrank earlier in this pass
            }
            Plan best_here = current;
            std::uint64_t best_here_bytes = current_bytes;
            for (int v = 0; v < field.values && !s.done; ++v) {
                if (v == field_value(current, field.id)) {
                    continue;
                }
                const auto candidate = with_field(current, field.id, v);
                if (candidate == current) {
                    continue;
                }
                auto r = try_plan(ev, s, candidate);
                if (!r) {
                    return std::unexpected(r.error());
                }
                if (*r && **r < best_here_bytes) {
                    best_here = candidate;
                    best_here_bytes = **r;
                }
            }
            if (best_here_bytes < current_bytes) {
                current = best_here;
                current_bytes = best_here_bytes;
                improved = true;
            }
            if (s.done) {
                return {};
            }
        }
        if (!improved) {
            // A full pass over every single-field neighbor found nothing better.
            auto r = stuck(ev, s, rng, restart, current, "local_optimum");
            if (!r) {
                return std::unexpected(r.error());
            }
            if (!*r) {
                return {};
            }
            current_bytes = **r;
        }
    }
    return {};
}

std::string plan_hex(const Plan &p) {
    std::string out;
    for (auto v = encode_seed(p); v != 0 || out.empty(); v >>= 4) {
        constexpr char digits[] = "0123456789abcdef";
        out.insert(out.begin(), digits[v & 15]);
    }
    return "0x" + out;
}

int cmd_search(const std::string &method, const Plan &start, const fs::path &input,
               std::uint64_t budget, std::uint64_t prng_seed, bool restart, int stall) {
    const auto w = wall_ms(), c = cpu_ms();
    auto files = read_tree(input);
    if (!files || files->empty()) {
        return fail(files ? bad("empty input") : files.error());
    }
    PlanEvaluator ev(*files, budget);
    SearchState s;
    s.best = start;
    Rng rng{prng_seed};
    auto r = method == "seed" ? search_seed(ev, s, rng, start, stall, restart)
                              : search_coord(ev, s, rng, start, restart);
    if (!r) {
        return fail(r.error());
    }
    if (s.proposals == 0 || s.best_bytes == UINT64_MAX) {
        return fail(Error{ErrorCode::resource_limit, "budget too small for the start plan"});
    }
    std::cout << "{\"schema\":\"seedarc.append-study.v4.r1\",\"mode\":\"search\",\"method\":"
              << json_string(method) << ",\"start_plan\":\"" << plan_hex(start)
              << "\",\"restart\":" << (restart ? "true" : "false") << ",\"stall\":" << stall
              << ",\"prng_seed\":" << prng_seed << ",\"files\":" << files->size()
              << ",\"input_bytes\":" << input_bytes_of(*files)
              << ",\"objective\":\"dictionaries+data+key_table+seed_record\",\"start_bytes\":"
              << s.start_bytes << ",\"best_plan\":\"" << plan_hex(s.best) << "\",\"best_rule\":\""
              << rule_names[s.best.rule] << "\",\"best_k\":" << static_cast<int>(s.best.k)
              << ",\"best_bytes\":" << s.best_bytes << ",\"proposals\":" << s.proposals
              << ",\"restarts\":" << s.restarts << ",\"stop\":\"" << s.stop << "\",\"trajectory\":["
              << s.trajectory << "]," << ev.stats_json() << ",\"call_wall_ms\":" << wall_ms() - w
              << ",\"call_cpu_ms\":" << cpu_ms() - c << "," << peak_memory_json() << "}\n";
    return std::cout ? 0 : 1;
}

std::string solid_plan_json(const Plan &p) {
    return "{\"rule\":" + std::to_string(p.rule) + ",\"groups\":" + std::to_string(p.k) +
           ",\"layout\":\"solid\",\"codec\":\"zstd\",\"level\":9}";
}

// Bounded full-pack trials; representation changes persistence only, never the search operator.
int cmd_solid_search(bool seeded, const Plan &start, const fs::path &input, const fs::path &output,
                     std::uint64_t budget, std::uint64_t prng, const std::string &policy) {
    const auto wall = wall_ms(), cpu = cpu_ms();
    auto files = read_tree(input);
    if (!files || files->empty()) {
        return fail(files ? bad("solid search needs a file") : files.error());
    }
    const Plan baseline;
    const auto input_bytes = input_bytes_of(*files);
    const auto unit = std::max<std::uint64_t>(input_bytes, 1);
    if (budget < unit || (start != baseline && budget - unit < unit)) {
        return fail(
            Error{ErrorCode::resource_limit, "budget must fund the start and strong default"});
    }
    std::map<Plan, std::uint64_t> cache;
    std::vector<std::byte> best_archive;
    Plan best = start;
    std::uint64_t best_size = UINT64_MAX, spent = 0, hits = 0;
    double encode_ms = 0, verify_ms = 0;
    std::string trajectory, trials;
    auto evaluate = [&](const Plan &plan) -> std::expected<std::optional<std::uint64_t>, Error> {
        if (const auto it = cache.find(plan); it != cache.end()) {
            ++hits;
            return std::optional{it->second};
        }
        if (unit > budget - spent) {
            return std::optional<std::uint64_t>{};
        }
        const auto ew = wall_ms();
        auto encoded =
            pack_bytes(seeded ? "seed-solid" : "explicit-solid", plan, *files, red_off, 0);
        encode_ms += wall_ms() - ew;
        if (!encoded) {
            return std::unexpected(encoded.error());
        }
        spent += unit;
        const auto vw = wall_ms();
        auto model = parse(encoded->bytes);
        if (!model) {
            return std::unexpected(model.error());
        }
        if (auto verified = byte_exact(*files, *model, encoded->bytes); !verified) {
            return std::unexpected(verified.error());
        }
        verify_ms += wall_ms() - vw;
        const auto size = static_cast<std::uint64_t>(encoded->bytes.size());
        cache.emplace(plan, size);
        trials += (trials.empty() ? "" : ",") + std::string{"{\"evaluation\":"} +
                  std::to_string(cache.size()) + ",\"plan\":" + solid_plan_json(plan) +
                  ",\"bytes\":" + std::to_string(size) +
                  ",\"spent_bytes\":" + std::to_string(spent) +
                  ",\"elapsed_wall_ms\":" + std::to_string(wall_ms() - wall) + "}";
        if (size < best_size ||
            (size == best_size && std::pair{plan.rule, plan.k} < std::pair{best.rule, best.k})) {
            best = plan;
            best_size = size;
            best_archive = std::move(encoded->bytes);
            trajectory += (trajectory.empty() ? "" : ",") + std::string{"{\"evaluations\":"} +
                          std::to_string(cache.size()) +
                          ",\"spent_bytes\":" + std::to_string(spent) +
                          ",\"best_bytes\":" + std::to_string(size) +
                          ",\"plan\":" + solid_plan_json(best) + "}";
        }
        return std::optional{size};
    };
    auto initial = evaluate(start);
    if (!initial || !*initial) {
        return fail(initial ? bad("start budget invariant") : initial.error());
    }
    const auto start_size = **initial;
    auto fixed = evaluate(baseline);
    if (!fixed || !*fixed) {
        return fail(fixed ? bad("baseline budget invariant") : fixed.error());
    }
    const auto baseline_size = **fixed;
    auto candidates = solid_candidates();
    std::map<Plan, std::uint64_t> rank;
    Rng rng{prng};
    for (const auto &plan : candidates) {
        rank.emplace(plan, rng.next());
    }
    if (policy == "near") {
        const auto distance = [&](const Plan &p) {
            return (p.rule != start.rule ? 1 : 0) + std::abs(int{p.k} - int{start.k});
        };
        std::ranges::sort(candidates, [&](const Plan &a, const Plan &b) {
            return std::tuple{distance(a), rank.at(a), a.rule, a.k} <
                   std::tuple{distance(b), rank.at(b), b.rule, b.k};
        });
    } else {
        std::ranges::sort(candidates, [](const Plan &a, const Plan &b) {
            return std::pair{a.rule, a.k} < std::pair{b.rule, b.k};
        });
    }
    std::string evaluated;
    // The ledger includes cache entries in a concrete-plan order, independent of representation.
    for (const auto &plan : candidates) {
        if (cache.contains(plan)) {
            continue;
        }
        auto trial = evaluate(plan);
        if (!trial) {
            return fail(trial.error());
        }
        if (!*trial) {
            break;
        }
    }
    for (const auto &[plan, size] : cache) {
        evaluated += (evaluated.empty() ? "" : ",") + std::string{"{\"plan\":"} +
                     solid_plan_json(plan) + ",\"bytes\":" + std::to_string(size) + "}";
    }
    auto best_model = parse(best_archive);
    if (!best_model) {
        return fail(best_model.error());
    }
    const auto publish = wall_ms();
    if (auto written = write_new(output, best_archive); !written) {
        return fail(written.error());
    }
    const auto publish_ms = wall_ms() - publish;
    std::cout << "{\"schema\":\"seedarc.solid-search.r1\",\"representation\":\""
              << (seeded ? "seed" : "explicit") << "\",\"policy\":" << json_string(policy)
              << ",\"search_revision\":\"near-r1\",\"prng_seed\":" << prng
              << ",\"start_plan\":" << solid_plan_json(start)
              << ",\"best_plan\":" << solid_plan_json(best) << ",\"strategy_seed\":"
              << (seeded ? std::to_string(encode_solid_seed(best)) : "null")
              << ",\"input_bytes\":" << input_bytes << ",\"accounting_unit_bytes\":" << unit
              << ",\"budget_bytes\":" << budget << ",\"budget_spent_bytes\":" << spent
              << ",\"evaluations\":" << cache.size() << ",\"cache_hits\":" << hits
              << ",\"domain_size\":" << candidates.size()
              << ",\"space_exhausted\":" << (cache.size() == candidates.size() ? "true" : "false")
              << ",\"start_bytes\":" << start_size << ",\"baseline_bytes\":" << baseline_size
              << ",\"best_bytes\":" << best_size << ",\"trajectory\":[" << trajectory
              << "],\"trials\":[" << trials << "],\"evaluated\":[" << evaluated
              << "],\"encode_wall_ms\":" << encode_ms << ",\"verify_wall_ms\":" << verify_ms
              << ",\"publish_wall_ms\":" << publish_ms << ",\"call_wall_ms\":" << wall_ms() - wall
              << ",\"call_cpu_ms\":" << cpu_ms() - cpu << ",\"byte_exact\":true,"
              << parts_json(*best_model, best_archive) << "," << peak_memory_json() << "}\n";
    return std::cout ? 0 : 1;
}

std::string frame_stats_json(const Model &m) {
    std::uint64_t largest = 0;
    std::string sizes;
    for (const auto &f : m.frames) {
        largest = std::max(largest, f.raw);
        sizes += (sizes.empty() ? "" : ",") + std::to_string(f.raw);
    }
    return "\"frame_count\":" + std::to_string(m.frames.size()) +
           ",\"max_frame_raw_bytes\":" + std::to_string(largest) + ",\"frame_raw_bytes\":[" +
           sizes + "]";
}

// ---- commands -----------------------------------------------------------------------------------
int cmd_pack(const std::string &mode, std::string_view plan_hex, const fs::path &input,
             const fs::path &out_path, std::optional<std::uint32_t> wanted, std::uint32_t cpb,
             std::uint64_t frame_limit = research::max_solid_bytes,
             const std::optional<SolidRecipe> &recipe = std::nullopt) {
    const bool strong = mode == "seed-solid" || mode == "explicit-solid";
    const auto bits = hex32(plan_hex);
    const auto plan = recipe ? std::optional{recipe->plan}
                      : mode == "explicit-solid" && plan_hex.find(':') != std::string_view::npos
                          ? parse_solid_explicit(plan_hex)
                      : strong ? (bits ? decode_solid_seed(*bits) : std::nullopt)
                               : plan_arg(plan_hex);
    if (!plan) {
        std::cerr << "not a canonical plan\n";
        return 2;
    }
    const auto w = wall_ms(), c = cpu_ms();
    auto files = read_tree(input);
    if (!files) {
        return fail(files.error());
    }
    const auto rw = wall_ms(), rc = cpu_ms();
    auto packed = pack_bytes(mode, *plan, *files, wanted, cpb, frame_limit,
                             recipe ? std::optional{recipe->fields} : std::nullopt);
    if (!packed) {
        return fail(packed.error());
    }
    const auto pack_wall = wall_ms() - rw, pack_cpu = cpu_ms() - rc;
    auto m = parse(packed->bytes);
    if (!m) {
        return fail(m.error());
    }
    if (auto r = byte_exact(*files, *m, packed->bytes); !r) {
        return fail(r.error());
    }
    if (auto r = write_new(out_path, packed->bytes); !r) {
        return fail(r.error());
    }
    std::string candidates;
    for (std::uint32_t red = red_off; red <= red_file; ++red) {
        candidates +=
            std::string{red == 0 ? "" : ","} + "\"" + redundancy_names[red] + "\":" +
            (packed->candidate_bytes[red] != 0 ? std::to_string(packed->candidate_bytes[red])
                                               : std::string{"null"});
    }
    std::cout << "{\"schema\":\"seedarc.append-study.v4.r1\",\"mode\":\"pack\",\"metadata\":"
              << json_string(mode) << ",\"plan_seed\":" << encode_seed(*plan) << ",\"rule\":\""
              << rule_names[plan->rule] << "\",\"k\":" << static_cast<int>(plan->k)
              << ",\"input_bytes\":" << input_bytes_of(*files) << ","
              << parts_json(*m, packed->bytes) << "," << frame_stats_json(*m)
              << ",\"frame_limit_bytes\":" << frame_limit << ",\"recipe\":"
              << (recipe ? json_string(recipe_text(*recipe)) : std::string{"null"})
              << ",\"recipe_seed\":"
              << (recipe ? std::to_string(encode_recipe_seed(*recipe)) : std::string{"null"})
              << ",\"redundancy_chosen_by\":\""
              << (packed->recommended ? "recommendation" : "user")
              << "\",\"recommendation_rule\":\"rec-r0 (provisional)\",\"candidate_bytes\":{"
              << candidates << "},\"pack_wall_ms\":" << pack_wall << ",\"pack_cpu_ms\":" << pack_cpu
              << ",\"call_wall_ms\":" << wall_ms() - w << ",\"call_cpu_ms\":" << cpu_ms() - c
              << ",\"byte_exact\":true," << peak_memory_json() << "}\n";
    return std::cout ? 0 : 1;
}

int cmd_append(const fs::path &old_path, const fs::path &new_dir, const fs::path &out_path,
               const std::string &how, const std::optional<Plan> &plan,
               std::uint64_t frame_limit = research::max_solid_bytes, bool require_strong = false,
               const std::optional<RecipeFields> &recipe = std::nullopt) {
    const auto w = wall_ms(), c = cpu_ms();
    auto old = read_file(old_path);
    if (!old) {
        return fail(old.error());
    }
    if (require_strong) {
        auto header = parse_header(*old);
        if (!header || header->revision != 5 || !header->solid()) {
            return fail(header ? bad("bounded append requires a revision5 solid archive")
                               : header.error());
        }
    }
    auto added = read_tree(new_dir);
    if (!added) {
        return fail(added.error());
    }
    const auto aw = wall_ms(), ac = cpu_ms();
    auto bytes = append_bytes(*old, *added, how, plan, frame_limit, recipe);
    if (!bytes) {
        return fail(bytes.error());
    }
    const auto append_wall = wall_ms() - aw, append_cpu = cpu_ms() - ac;
    auto m = parse(*bytes);
    if (!m) {
        return fail(m.error());
    }
    if (auto checked = byte_exact(*added, *m, *bytes, false); !checked) {
        return fail(checked.error());
    }
    if (auto r = write_new(out_path, *bytes); !r) {
        return fail(r.error());
    }
    const bool prefix =
        bytes->size() >= old->size() && std::equal(old->begin(), old->end(), bytes->begin());
    std::cout << "{\"schema\":\"seedarc.append-study.v4.r1\",\"mode\":\"append\",\"how\":"
              << json_string(how) << ",\"generation_plan\":"
              << (plan ? std::to_string(encode_seed(*plan)) : std::string{"null"})
              << ",\"appended_files\":" << added->size()
              << ",\"appended_bytes\":" << input_bytes_of(*added)
              << ",\"old_container_bytes\":" << old->size()
              << ",\"old_is_prefix\":" << (prefix ? "true" : "false") << ","
              << parts_json(*m, *bytes) << "," << frame_stats_json(*m)
              << ",\"frame_limit_bytes\":" << frame_limit << ",\"new_generation_recipe\":"
              << (m->gens.back().recipe
                      ? json_string(recipe_text({m->gens.back().plan, *m->gens.back().recipe}))
                      : std::string{"null"})
              << ",\"old_archive_bytes_read\":" << old->size()
              << ",\"old_archive_bytes_copied\":" << (prefix ? old->size() : 0)
              << ",\"old_plaintext_bytes_recompressed\":"
              << (how == "regroup" ? std::string{"null"} : std::string{"0"})
              << ",\"archive_bytes_written\":" << bytes->size()
              << ",\"append_wall_ms\":" << append_wall << ",\"append_cpu_ms\":" << append_cpu
              << ",\"call_wall_ms\":" << wall_ms() - w << ",\"call_cpu_ms\":" << cpu_ms() - c
              << ",\"appended_byte_exact\":true," << peak_memory_json() << "}\n";
    return std::cout ? 0 : 1;
}

int cmd_verify(const fs::path &container, const fs::path &input) {
    const auto w = wall_ms(), c = cpu_ms();
    auto bytes = read_file(container);
    if (!bytes) {
        return fail(bytes.error());
    }
    auto m = parse(*bytes);
    if (!m) {
        return fail(m.error());
    }
    const auto load_wall = wall_ms() - w, load_cpu = cpu_ms() - c;
    const auto iw = wall_ms(), ic = cpu_ms();
    auto files = read_tree(input);
    if (!files) {
        return fail(files.error());
    }
    const auto input_wall = wall_ms() - iw, input_cpu = cpu_ms() - ic;
    const auto vw = wall_ms(), vc = cpu_ms();
    if (auto checked = byte_exact(*files, *m, *bytes); !checked) {
        return fail(checked.error());
    }
    const auto decode_wall = load_wall + wall_ms() - vw;
    const auto decode_cpu = load_cpu + cpu_ms() - vc;
    std::cout << "{\"schema\":\"seedarc.append-study.v4.r1\",\"mode\":\"verify\",\"byte_exact\":"
                 "true,\"files\":"
              << files->size() << ",\"generations\":" << m->gens.size() << ",\"redundancy\":\""
              << redundancy_names[m->header.redundancy] << "\",\"decode_wall_ms\":" << decode_wall
              << ",\"decode_cpu_ms\":" << decode_cpu << ",\"input_read_wall_ms\":" << input_wall
              << ",\"input_read_cpu_ms\":" << input_cpu
              << ",\"verification_mode\":\"incremental_crc_and_compare\"," << peak_memory_json()
              << "}\n";
    return std::cout ? 0 : 1;
}

// Cold single-file extraction: read and parse the container, decode only what that file needs.
int cmd_peek(const fs::path &container, const fs::path &input, std::size_t n) {
    auto files = read_tree(input);
    if (!files || files->empty()) {
        return fail(files ? bad("empty input") : files.error());
    }
    std::vector<double> times;
    const auto step = std::max<std::size_t>(1, files->size() / n);
    for (std::size_t i = 0; i < files->size() && times.size() < n; i += step) {
        const auto &want = (*files)[i];
        const auto w = wall_ms();
        auto bytes = read_file(container);
        if (!bytes) {
            return fail(bytes.error());
        }
        auto m = parse(*bytes);
        if (!m) {
            return fail(m.error());
        }
        const auto it = std::ranges::find(m->entries, want.path, &Entry::path);
        if (it == m->entries.end()) {
            return fail(bad("file missing"));
        }
        std::vector<std::byte> data;
        if (m->header.solid()) {
            auto raw = decode_frame(*m, m->frames[it->frame], *bytes);
            if (!raw) {
                return fail(raw.error());
            }
            const auto view = std::span{*raw}.subspan(static_cast<std::size_t>(it->frame_offset),
                                                      static_cast<std::size_t>(it->size));
            data.assign(view.begin(), view.end());
        } else {
            DecoderCache cache;
            auto r = decode_blocks(*it, m->gens[it->gen].groups[it->group], *bytes, cache);
            if (!r) {
                return fail(r.error());
            }
            data = std::move(*r);
        }
        times.push_back(wall_ms() - w);
        if (data != want.data) {
            return fail(bad("single-file extraction mismatch"));
        }
    }
    std::ranges::sort(times);
    const auto q = [&](double p) {
        return times[std::min(
            times.size() - 1,
            static_cast<std::size_t>(p * static_cast<double>(times.size() - 1) + 0.5))];
    };
    std::cout << "{\"schema\":\"seedarc.append-study.v4.r1\",\"mode\":\"peek\",\"byte_exact\":true,"
                 "\"samples\":"
              << times.size() << ",\"median_ms\":" << q(0.5) << ",\"p90_ms\":" << q(0.9)
              << ",\"max_ms\":" << times.back() << "," << peak_memory_json() << "}\n";
    return std::cout ? 0 : 1;
}

// Recovery of `cut` leading bytes, checked against the input: every recovered file must be
// byte-exact (recovering a wrong file is an error, recovering fewer is a measurement).
struct CutResult {
    std::uint64_t files = 0, bytes = 0, from_trailer = 0, from_records = 0, unreadable = 0;
};
std::expected<CutResult, Error>
recover_checked(std::span<const std::byte> bytes,
                const std::map<std::string_view, const InputFile *> &by_path,
                const Presence *presence = nullptr) {
    auto r = recover(bytes, presence);
    if (!r) {
        return std::unexpected(r.error());
    }
    CutResult out{0, 0, r->from_trailer, r->from_records, r->unreadable};
    for (const auto &[path, data] : r->files) {
        const auto it = by_path.find(path);
        if (it == by_path.end() || it->second->data != data) {
            return std::unexpected(bad("recovery produced a file that is not in the input"));
        }
        ++out.files;
        out.bytes += data.size();
    }
    return out;
}
std::map<std::string_view, const InputFile *> index_files(const std::vector<InputFile> &files) {
    std::map<std::string_view, const InputFile *> by;
    for (const auto &f : files) {
        by.emplace(f.path, &f);
    }
    return by;
}

int cmd_partial(const fs::path &container, const fs::path &input) {
    auto bytes = read_file(container);
    if (!bytes) {
        return fail(bytes.error());
    }
    auto files = read_tree(input);
    if (!files) {
        return fail(files.error());
    }
    const auto by_path = index_files(*files);
    const auto total = input_bytes_of(*files);
    std::cout
        << "{\"schema\":\"seedarc.append-study.v4.r1\",\"mode\":\"partial\",\"container_bytes\":"
        << bytes->size() << ",\"input_files\":" << files->size() << ",\"input_bytes\":" << total
        << ",\"cuts\":[";
    std::vector<std::uint64_t> cuts;
    for (int pct = 5; pct <= 95; pct += 5) {
        cuts.push_back(bytes->size() * static_cast<std::uint64_t>(pct) / 100);
    }
    cuts.push_back(bytes->size() - 1);
    cuts.push_back(bytes->size());
    for (std::size_t i = 0; i < cuts.size(); ++i) {
        const auto w = wall_ms();
        auto r =
            recover_checked(std::span{*bytes}.first(static_cast<std::size_t>(cuts[i])), by_path);
        if (!r) {
            std::cout << "]}\n";
            return fail(r.error());
        }
        std::cout << (i == 0 ? "" : ",") << "{\"kept_bytes\":" << cuts[i]
                  << ",\"recovered_files\":" << r->files << ",\"recovered_bytes\":" << r->bytes
                  << ",\"from_trailer\":" << r->from_trailer
                  << ",\"from_records\":" << r->from_records
                  << ",\"unreadable_listed\":" << r->unreadable
                  << ",\"recover_wall_ms\":" << wall_ms() - w << "}";
    }
    std::cout << "]," << peak_memory_json() << "}\n";
    return std::cout ? 0 : 1;
}

std::string volumes_json(const std::vector<Volume> &vols) {
    std::uint64_t files = 0, carry = 0;
    for (const auto &v : vols) {
        files += volume_file(v).size();
        for (const auto &bytes : v.carry | std::views::values) {
            carry += bytes.size();
        }
    }
    return "\"volumes_written\":" + std::to_string(vols.size()) +
           ",\"volume_file_bytes\":" + std::to_string(files) +
           ",\"carried_bytes\":" + std::to_string(carry);
}

int cmd_split(const fs::path &container, std::uint64_t nominal, const fs::path &dir,
              std::uint32_t mode) {
    auto bytes = read_file(container);
    if (!bytes) {
        return fail(bytes.error());
    }
    auto m = parse(*bytes);
    if (!m) {
        return fail(m.error());
    }
    auto vols = make_volumes(*bytes, *m, 0, 0, nominal, mode, 0);
    if (!vols) {
        return fail(vols.error());
    }
    std::error_code ec;
    if (fs::exists(dir) || !fs::create_directory(dir, ec) || ec) {
        return fail(Error{ErrorCode::output_conflict, "volume directory exists or cannot be made"});
    }
    if (auto r = write_volumes(dir, *vols); !r) {
        return fail(r.error());
    }
    std::cout
        << "{\"schema\":\"seedarc.append-study.v4.r1\",\"mode\":\"split\",\"container_bytes\":"
        << bytes->size() << ",\"volume_bytes\":" << nominal << ",\"carry\":\"" << carry_names[mode]
        << "\"," << volumes_json(*vols) << "}\n";
    return std::cout ? 0 : 1;
}

int cmd_vappend(const fs::path &dir, const fs::path &new_dir, const std::optional<Plan> &plan) {
    const auto w = wall_ms(), c = cpu_ms();
    auto set = read_volumes(dir);
    if (!set) {
        return fail(set.error());
    }
    auto old = join(*set);
    if (!old) {
        return fail(old.error());
    }
    auto added = read_tree(new_dir);
    if (!added) {
        return fail(added.error());
    }
    auto bytes = append_bytes(*old, *added, "segment", plan);
    if (!bytes) {
        return fail(bytes.error());
    }
    auto m = parse(*bytes);
    if (!m) {
        return fail(m.error());
    }
    auto decoded = decode_all(*m, *bytes);
    if (!decoded) {
        return fail(decoded.error());
    }
    for (const auto &f : *added) {
        const auto it = decoded->find(f.path);
        if (it == decoded->end() || it->second != f.data) {
            return fail(bad("appended file does not round trip"));
        }
    }
    const auto &last = set->volumes.rbegin()->second;
    auto vols = make_volumes(*bytes, *m, old->size(), last.index + 1, set->nominal, set->carry_mode,
                             last.file_crc);
    if (!vols) {
        return fail(vols.error());
    }
    if (auto r = write_volumes(dir, *vols); !r) {
        return fail(r.error());
    }
    std::cout << "{\"schema\":\"seedarc.append-study.v4.r1\",\"mode\":\"vappend\",\"old_volumes\":"
              << set->volumes.size()
              << ",\"old_volumes_rewritten\":0,\"first_new_volume\":" << last.index + 1 << ","
              << volumes_json(*vols) << ",\"appended_files\":" << added->size()
              << ",\"call_wall_ms\":" << wall_ms() - w << ",\"call_cpu_ms\":" << cpu_ms() - c
              << ",\"appended_byte_exact\":true}\n";
    return std::cout ? 0 : 1;
}

int cmd_vverify(const fs::path &dir, const fs::path &input) {
    auto set = read_volumes(dir);
    if (!set) {
        return fail(set.error());
    }
    auto bytes = join(*set);
    if (!bytes) {
        return fail(bytes.error());
    }
    auto m = parse(*bytes);
    if (!m) {
        return fail(m.error());
    }
    auto files = read_tree(input);
    if (!files) {
        return fail(files.error());
    }
    if (auto r = byte_exact(*files, *m, *bytes); !r) {
        return fail(r.error());
    }
    std::cout << "{\"schema\":\"seedarc.append-study.v4.r1\",\"mode\":\"vverify\",\"byte_exact\":"
                 "true,\"volumes\":"
              << set->volumes.size() << ",\"files\":" << files->size()
              << ",\"generations\":" << m->gens.size() << "}\n";
    return std::cout ? 0 : 1;
}

// Recovery with each single volume missing, against the input. Also reports how many files
// have data inside the missing volume (they cannot come back without a recovery record).
int cmd_vpartial(const fs::path &dir, const fs::path &input) {
    auto set = read_volumes(dir);
    if (!set) {
        return fail(set.error());
    }
    auto whole = join(*set);
    if (!whole) {
        return fail(whole.error());
    }
    auto m = parse(*whole);
    if (!m) {
        return fail(m.error());
    }
    auto files = read_tree(input);
    if (!files) {
        return fail(files.error());
    }
    const auto by_path = index_files(*files);
    std::cout << "{\"schema\":\"seedarc.append-study.v4.r1\",\"mode\":\"vpartial\",\"volumes\":"
              << set->volumes.size() << ",\"carry\":\"" << carry_names[set->carry_mode]
              << "\",\"redundancy\":\"" << redundancy_names[m->header.redundancy]
              << "\",\"input_files\":" << files->size() << ",\"missing\":[";
    bool first = true;
    for (const auto &[index, v] : set->volumes) {
        VolumeSet without = *set;
        without.volumes.erase(index);
        if (without.volumes.empty()) {
            continue;
        }
        auto a = assemble(without);
        if (!a) {
            std::cout << "]}\n";
            return fail(a.error());
        }
        auto r = recover_checked(a->first, by_path, &a->second);
        if (!r) {
            std::cout << "]}\n";
            return fail(r.error());
        }
        std::uint64_t touching = 0;
        const auto hits = [&](std::uint64_t off, std::uint64_t len) {
            return len != 0 && off < v.end() && off + len > v.start;
        };
        for (const auto &e : m->entries) {
            const bool hit = m->header.solid()
                                 ? hits(m->frames[e.frame].offset, m->frames[e.frame].stored)
                                 : std::ranges::any_of(e.blocks, [&](const Block &b) {
                                       return hits(b.offset, b.stored);
                                   });
            touching += hit ? 1 : 0;
        }
        std::cout << (first ? "" : ",") << "{\"volume\":" << index
                  << ",\"recovered_files\":" << r->files << ",\"recovered_bytes\":" << r->bytes
                  << ",\"files_with_data_in_volume\":" << touching
                  << ",\"from_trailer\":" << r->from_trailer
                  << ",\"from_records\":" << r->from_records << "}";
        first = false;
    }
    std::cout << "]}\n";
    return std::cout ? 0 : 1;
}

// Damage, prefix, truncation and recompute self-test on a real input split into two generations.
int cmd_selftest(const fs::path &input) {
    auto files = read_tree(input);
    if (!files || files->size() < 4) {
        return fail(files ? bad("selftest needs at least 4 files") : files.error());
    }
    const auto split = static_cast<std::ptrdiff_t>(files->size() * 3 / 4);
    const std::vector<InputFile> first(files->begin(), files->begin() + split);
    const std::vector<InputFile> second(files->begin() + split, files->end());
    const auto by_path = index_files(*files);
    Plan plan;
    plan.rule = 0;
    plan.k = 3;
    plan.d = {2, 1, 0};
    plan.t = {1, 0, 0};
    plan = canonical(plan);
    Plan plan_b; // a different Seed for the second generation (queue variant B)
    plan_b.rule = 2;
    plan_b.k = 2;
    plan_b.d = {1, 1};
    plan_b = canonical(plan_b);
    // The append command compares only new source files, but must still decode and CRC-check
    // old payloads. A subset comparator that simply skips old files would accept this damage.
    {
        const auto plain_plan = canonical(Plan{});
        auto packed = pack_bytes("seed", plain_plan, first, red_off, min_checkpoint_bytes);
        if (!packed) {
            return fail(packed.error());
        }
        auto old_model = parse(packed->bytes);
        if (!old_model) {
            return fail(old_model.error());
        }
        const auto file = std::ranges::find_if(old_model->entries,
                                               [](const Entry &e) { return !e.blocks.empty(); });
        if (file == old_model->entries.end()) {
            return fail(bad("selftest: no old payload for subset verification"));
        }
        auto damaged = std::move(packed->bytes);
        damaged[static_cast<std::size_t>(file->blocks.front().offset)] ^= std::byte{1};
        auto grown = append_bytes(damaged, second, "segment", std::nullopt);
        if (!grown) {
            return fail(grown.error());
        }
        auto model = parse(*grown);
        if (!model || byte_exact(second, *model, *grown, false)) {
            return fail(bad("selftest: subset comparison accepted corrupt old payload"));
        }
    }
    std::uint64_t checks = 0, tried = 0, rejected = 0, stale_checked = 0, cut_checks = 0;
    constexpr std::uint32_t cpb = min_checkpoint_bytes;
    for (const auto *mode : {"seed", "explicit", "solid"}) {
        for (std::uint32_t red = red_off; red <= red_file; ++red) {
            std::vector<std::string> hows{"segment", "plan"};
            if (std::string{mode} == "solid") {
                hows.emplace_back("regroup");
            }
            for (const auto &hw : hows) {
                auto packed = pack_bytes(mode, plan, first, red, cpb);
                if (!packed) {
                    return fail(packed.error());
                }
                const auto &old = packed->bytes;
                auto grown = append_bytes(old, second, hw == "regroup" ? "regroup" : "segment",
                                          hw == "plan" ? std::optional{plan_b} : std::nullopt);
                if (!grown) {
                    return fail(grown.error());
                }
                const auto &good = *grown;
                auto m = parse(good);
                const bool fresh = hw == "regroup";
                if (!m || !byte_exact(*files, *m, good) || m->gens.size() != (fresh ? 1U : 2U) ||
                    m->header.redundancy != red) {
                    return fail(bad("selftest: grown container does not restore every file"));
                }
                const bool prefix = std::equal(old.begin(), old.end(), good.begin());
                if (prefix == fresh) {
                    return fail(bad("selftest: prefix property"));
                }
                if (hw == "plan" && m->gens[1].body == m->gens[0].body) {
                    return fail(bad("selftest: second generation did not get its own record"));
                }
                if (append_bytes(good, second, "segment", std::nullopt)) {
                    return fail(bad("selftest: duplicate append accepted"));
                }
                checks += 4;
                // Bit flips: a superseded trailer is never read again (damage there must leave
                // the container restoring correctly); damage anywhere else must be refused.
                std::size_t stale_begin = good.size(), stale_end = good.size();
                if (prefix) {
                    stale_begin = static_cast<std::size_t>(parse(old)->trailer_offset);
                    stale_end = old.size();
                }
                std::vector<std::size_t> offsets{0,
                                                 8,
                                                 12,
                                                 16,
                                                 20,
                                                 24,
                                                 28,
                                                 header_bytes,
                                                 good.size() / 2,
                                                 good.size() - 1,
                                                 good.size() - 9,
                                                 good.size() - 17};
                for (std::size_t at = 33; at < good.size(); at += good.size() / 40 + 1) {
                    offsets.push_back(at);
                }
                for (const auto &g : m->gens) { // every generation record
                    offsets.push_back(static_cast<std::size_t>(g.record_offset) + 3);
                    offsets.push_back(static_cast<std::size_t>(g.record_offset) + record_overhead +
                                      g.body.size() / 2);
                }
                if (stale_end > stale_begin) {
                    offsets.push_back(stale_begin);
                    offsets.push_back(stale_end - 1);
                }
                const auto accepted = [&](std::span<const std::byte> bytes) {
                    auto c = parse(bytes);
                    return c && byte_exact(*files, *c, bytes).has_value();
                };
                for (const auto at : offsets) {
                    auto copy = good;
                    copy[at] ^= std::byte{0x5a};
                    if (at >= stale_begin && at < stale_end) {
                        ++stale_checked;
                        if (!accepted(copy)) {
                            return fail(bad(
                                "selftest: damage in a superseded trailer broke the container"));
                        }
                        continue;
                    }
                    ++tried;
                    rejected += accepted(copy) ? 0 : 1;
                }
                for (const auto keep :
                     {std::size_t{0}, header_bytes, good.size() / 3, good.size() - 1}) {
                    ++tried;
                    rejected += accepted(std::span{good}.first(keep)) ? 0 : 1;
                }
                // Truncation: recovery never yields a wrong file, never loses files as more bytes
                // arrive, and with redundancy records gets everything once only the footer is
                // missing. Without them, the superseded trailer still yields generation 0.
                std::uint64_t prev = 0;
                for (int pct = 0; pct <= 100; pct += 10) {
                    const auto keep = pct == 100
                                          ? good.size() - 1
                                          : good.size() * static_cast<std::size_t>(pct) / 100;
                    auto r = recover_checked(std::span{good}.first(keep), by_path);
                    if (!r || r->files < prev) {
                        return fail(r ? bad("selftest: recovery shrank with more bytes")
                                      : r.error());
                    }
                    prev = r->files;
                    ++cut_checks;
                }
                const auto expect_last = red != red_off ? files->size()
                                         : prefix       ? first.size()
                                                        : 0U;
                if (prev != expect_last) {
                    return fail(bad("selftest: recovery without the footer is incomplete"));
                }
                // A damaged byte in the middle of data or records: recovery still works and
                // returns only correct files.
                auto damaged = good;
                damaged[good.size() / 2] ^= std::byte{0x5a};
                if (!recover_checked(damaged, by_path)) {
                    return fail(bad("selftest: recovery failed on a damaged container"));
                }
                ++cut_checks;
            }
        }
    }
    // Volumes: any damaged header or carry byte is refused; damaged data is caught when the
    // joined container is read; a set round trips and joins back to the same bytes.
    std::uint64_t volume_tried = 0, volume_rejected = 0;
    {
        auto packed = pack_bytes("seed", plan, *files, red_file, cpb);
        if (!packed) {
            return fail(packed.error());
        }
        auto m = parse(packed->bytes);
        auto vols = m ? make_volumes(packed->bytes, *m, 0, 0, min_volume_bytes, carry_dicts, 0)
                      : std::unexpected(m.error());
        if (!vols || vols->size() < 2) {
            return fail(bad("selftest: expected several volumes"));
        }
        VolumeSet set;
        set.id = vols->front().id;
        set.nominal = min_volume_bytes;
        set.carry_mode = carry_dicts;
        for (auto &v : *vols) {
            auto file = volume_file(v);
            auto again = parse_volume(file);
            if (!again || volume_file(*again) != file) {
                return fail(bad("selftest: volume does not round trip"));
            }
            const auto head = file.size() - v.data.size();
            for (const auto at : {std::size_t{0}, std::size_t{9}, std::size_t{20}, std::size_t{30},
                                  std::size_t{50}, volume_fixed, head / 2, head - 1}) {
                auto copy = file;
                copy[at] ^= std::byte{0x5a};
                ++volume_tried;
                volume_rejected += parse_volume(copy) ? 0 : 1;
            }
            set.volumes.emplace(v.index, std::move(*again));
        }
        auto joined = join(set);
        if (!joined || *joined != packed->bytes) {
            return fail(bad("selftest: volumes do not join back"));
        }
        auto bent = set;
        bent.volumes.begin()->second.data.back() ^= std::byte{0x5a};
        ++volume_tried;
        auto bj = join(bent);
        auto bm = bj ? parse(*bj) : std::unexpected(bj.error());
        volume_rejected += bm && byte_exact(*files, *bm, *bj) ? 0 : 1;
    }
    tried += volume_tried;
    rejected += volume_rejected;
    std::cout << "{\"schema\":\"seedarc.append-study.v4.r1\",\"mode\":\"selftest\",\"checks\":"
              << checks << ",\"volume_damage_tried\":" << volume_tried
              << ",\"damaged_tried\":" << tried << ",\"damaged_rejected\":" << rejected
              << ",\"superseded_trailer_bytes_checked_inert\":" << stale_checked
              << ",\"recovery_checks\":" << cut_checks << "}\n";
    return rejected == tried ? 0 : 1;
}

// Compare encoded frames, independently of offsets and representation metadata.
bool same_solid_payload(const Model &a, std::span<const std::byte> ab, const Model &b,
                        std::span<const std::byte> bb) {
    if (!a.header.solid() || !b.header.solid() || a.frames.size() != b.frames.size() ||
        a.entries.size() != b.entries.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.entries.size(); ++i) {
        const auto &x = a.entries[i], &y = b.entries[i];
        if (x.path != y.path || x.size != y.size || x.gen != y.gen || x.group != y.group ||
            x.frame != y.frame || x.frame_offset != y.frame_offset || x.file_crc != y.file_crc) {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.frames.size(); ++i) {
        const auto &x = a.frames[i], &y = b.frames[i];
        if (x.gen != y.gen || x.group != y.group || x.raw != y.raw || x.stored != y.stored ||
            !std::ranges::equal(
                ab.subspan(static_cast<std::size_t>(x.offset), static_cast<std::size_t>(x.stored)),
                bb.subspan(static_cast<std::size_t>(y.offset),
                           static_cast<std::size_t>(y.stored)))) {
            return false;
        }
    }
    return true;
}

int cmd_payload_compare(const fs::path &left, const fs::path &right) {
    auto a = read_file(left), b = read_file(right);
    if (!a || !b) {
        return fail(!a ? a.error() : b.error());
    }
    auto am = parse(*a), bm = parse(*b);
    if (!am || !bm) {
        return fail(!am ? am.error() : bm.error());
    }
    const auto equal = same_solid_payload(*am, *a, *bm, *b);
    std::uint64_t data = 0;
    for (const auto &f : am->frames) {
        data += f.stored;
    }
    std::cout << "{\"schema\":\"seedarc.solid-payload-compare.r1\",\"equal\":"
              << (equal ? "true" : "false") << ",\"frames\":" << am->frames.size()
              << ",\"payload_bytes\":" << data << ",\"left_container_bytes\":" << a->size()
              << ",\"right_container_bytes\":" << b->size() << "}\n";
    return equal ? 0 : 1;
}

int cmd_solid_selftest(const fs::path &input) {
    auto files = read_tree(input);
    if (!files || files->size() < 4) {
        return fail(files ? bad("solid selftest needs four files") : files.error());
    }
    std::uint64_t checks = 0;
    for (auto bits : {0U, 6U, 11U, 31U}) {
        auto plan = decode_solid_seed(bits);
        if (!plan || encode_solid_seed(*plan) != bits ||
            decode_solid_explicit(encode_solid_explicit(*plan)) != plan) {
            return fail(bad("solid mapping fixed vector failed"));
        }
        const auto half = files->size() / 2;
        std::vector<InputFile> initial(files->begin(),
                                       files->begin() + static_cast<std::ptrdiff_t>(half));
        std::vector<InputFile> added(files->begin() + static_cast<std::ptrdiff_t>(half),
                                     files->end());
        auto a = pack_bytes("seed-solid", *plan, initial, red_off, 0);
        auto b = pack_bytes("explicit-solid", *plan, initial, red_off, 0);
        auto legacy = pack_bytes("solid", *plan, initial, red_off, 0);
        if (!a || !b || !legacy) {
            return fail(bad("solid selftest pack failed"));
        }
        auto check_pair = [&](const std::vector<std::byte> &x, const std::vector<std::byte> &y,
                              const std::vector<InputFile> &original) {
            auto xm = parse(x), ym = parse(y);
            return xm && ym && byte_exact(original, *xm, x) && byte_exact(original, *ym, y) &&
                   same_solid_payload(*xm, x, *ym, y);
        };
        if (!check_pair(a->bytes, b->bytes, initial) ||
            !check_pair(a->bytes, legacy->bytes, initial)) {
            return fail(bad("solid pack changed the codec payload"));
        }
        auto again = pack_bytes("seed-solid", *plan, initial, red_off, 0);
        if (!again || again->bytes != a->bytes) {
            return fail(bad("solid pack is not deterministic"));
        }
        for (const auto &how : {"segment", "regroup"}) {
            auto x = append_bytes(a->bytes, added, how, std::nullopt);
            auto y = append_bytes(b->bytes, added, how, std::nullopt);
            if (!x || !y || !check_pair(*x, *y, *files)) {
                return fail(bad("solid append changed payload or roundtrip"));
            }
            if (std::string_view{how} == "segment" &&
                (x->size() < a->bytes.size() ||
                 !std::equal(a->bytes.begin(), a->bytes.end(), x->begin()))) {
                return fail(bad("solid append lost old prefix"));
            }
            auto repeated = append_bytes(a->bytes, added, how, std::nullopt);
            if (!repeated || *repeated != *x) {
                return fail(bad("solid append is not deterministic"));
            }
            ++checks;
        }
        const auto next = decode_solid_seed(bits == 6 ? 11U : 6U);
        auto x = append_bytes(a->bytes, added, "segment", next);
        auto y = append_bytes(b->bytes, added, "segment", next);
        if (!x || !y || !check_pair(*x, *y, *files)) {
            return fail(bad("solid new-generation plan mismatch"));
        }
        auto model = parse(a->bytes);
        auto body = gen_body(model->gens.front());
        body[16] = std::byte{99};
        if (parse_gen_body(body, model->header)) {
            return fail(bad("unknown solid seed revision accepted"));
        }
        body = gen_body(model->gens.front());
        body[20] = std::byte{32};
        if (parse_gen_body(body, model->header)) {
            return fail(bad("reserved solid seed bits accepted"));
        }
        auto invalid = a->bytes;
        invalid[8] = std::byte{4}; // v4 must still reject the combined seed+solid flags.
        if (parse(invalid)) {
            return fail(bad("v4 accepted the new feature flags"));
        }
        auto corrupt = a->bytes;
        corrupt[static_cast<std::size_t>(model->frames.front().offset)] ^= std::byte{1};
        if (auto parsed = parse(corrupt); parsed && byte_exact(initial, *parsed, corrupt)) {
            return fail(bad("corrupt solid frame accepted"));
        }
        ++checks;
    }
    for (auto invalid : {1U, 2U, 3U, 32U, 64U, UINT32_MAX}) {
        if (decode_solid_seed(invalid)) {
            return fail(bad("noncanonical solid seed accepted"));
        }
    }
    std::cout << "{\"schema\":\"seedarc.solid-selftest.r1\",\"checks\":" << checks
              << ",\"payload_equal\":true,\"deterministic\":true,\"strategy_revision\":2}\n";
    return 0;
}

std::optional<std::uint32_t> redundancy_arg(std::string_view s, bool &ok) {
    ok = true;
    for (std::uint32_t red = red_off; red <= red_file; ++red) {
        if (s == redundancy_names[red]) {
            return red;
        }
    }
    ok = s == "auto";
    return std::nullopt;
}
#include "bounded_solid.hpp"
#include "bounded_extract.hpp"
#include "recipe_commands.hpp"
#include "drift_probe.hpp"
#include "codec_probe.hpp"
#include "dup_probe.hpp"
#include "cut_probe.hpp"

// Command-line arguments as UTF-8. On Windows argv is in the ANSI code page, which cannot carry
// every path (archive-internal paths such as 源/文件.txt included), so the wide command line is
// converted instead.
std::optional<std::vector<std::string>> utf8_arguments([[maybe_unused]] int argc,
                                                       [[maybe_unused]] char *argv[]) {
#ifdef _WIN32
    int count = 0;
    const auto free_args = [](wchar_t **p) { LocalFree(p); };
    const std::unique_ptr<wchar_t *, decltype(free_args)> wide(
        CommandLineToArgvW(GetCommandLineW(), &count), free_args);
    if (!wide || count < 1) {
        return std::nullopt;
    }
    std::vector<std::string> out;
    for (int i = 0; i < count; ++i) {
        const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.get()[i], -1,
                                             nullptr, 0, nullptr, nullptr);
        if (size <= 0) {
            return std::nullopt;
        }
        std::string text(static_cast<std::size_t>(size), '\0');
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.get()[i], -1, text.data(), size,
                                nullptr, nullptr) != size) {
            return std::nullopt;
        }
        text.pop_back();
        out.push_back(std::move(text));
    }
    return out;
#else
    return std::vector<std::string>(argv, argv + argc);
#endif
}

} // namespace

// append4_fuzz.cpp includes this file for its parsers and has its own entry point; the command
// dispatcher is still compiled there under another name, so every command stays referenced.
#ifdef SEEDARC_APPEND4_NO_MAIN
int append4_study_main(int argc, char *argv[]) {
#else
int main(int argc, char *argv[]) {
#endif
    const auto owned = utf8_arguments(argc, argv);
    if (!owned) {
        std::cerr << "command line is not valid Unicode\n";
        return 2;
    }
    std::vector<std::string_view> a(owned->begin(), owned->end());
    // Leading option for every writing command; it changes speed and memory, never the bytes.
    if (a.size() >= 3 && a[1] == "--jobs") {
        const auto jobs = number(a[2]);
        if (!jobs || *jobs < 1 || *jobs > max_encode_jobs || std::to_string(*jobs) != a[2]) {
            std::cerr << usage;
            return 2;
        }
        encode_jobs = static_cast<std::size_t>(*jobs);
        a.erase(a.begin() + 1, a.begin() + 3);
    }
    if (a.size() == 6 && a[1] == "bounded-pack") {
        const auto cap = number(a[3]);
        if (!cap || *cap == 0 || *cap > research::max_solid_bytes ||
            (a[2] != "seed" && a[2] != "explicit")) {
            std::cerr << usage;
            return 2;
        }
        return cmd_pack(a[2] == "seed" ? "seed-solid" : "explicit-solid", "0x0", native(a[4]),
                        native(a[5]), red_off, 0, *cap);
    }
    if (a.size() == 6 && a[1] == "bounded-append") {
        const auto cap = number(a[2]);
        if (!cap || *cap == 0 || *cap > research::max_solid_bytes) {
            std::cerr << usage;
            return 2;
        }
        return cmd_append(native(a[3]), native(a[4]), native(a[5]), "segment", std::nullopt, *cap,
                          true);
    }
    if (a.size() == 5 && a[1] == "access-profile") {
        const auto count = number(a[4]);
        if (!count || *count == 0 || *count > 1000) {
            return 2;
        }
        return cmd_access_profile(native(a[2]), native(a[3]), static_cast<std::size_t>(*count));
    }
    if (a.size() == 4 && a[1] == "payload-fault-profile") {
        return cmd_payload_fault_profile(native(a[2]), native(a[3]));
    }
    if (a.size() == 6 && a[1] == "recipe-pack") {
        return cmd_recipe_pack(a[2], a[3], native(a[4]), native(a[5]));
    }
    if ((a.size() == 5 || a.size() == 6) && a[1] == "recipe-append") {
        return cmd_recipe_append(native(a[2]), native(a[3]), native(a[4]),
                                 a.size() == 6 ? std::optional{a[5]} : std::nullopt);
    }
    if (a.size() == 5 && a[1] == "drift-probe") {
        const auto zstd_level = number(a[4]);
        if (!zstd_level || *zstd_level < 1 || *zstd_level > 19) {
            return 2;
        }
        return cmd_drift_probe(native(a[2]), native(a[3]), static_cast<std::uint32_t>(*zstd_level));
    }
    if ((a.size() == 3 || a.size() == 4) && a[1] == "consolidate-check") {
        return cmd_consolidate(native(a[2]), std::nullopt,
                               a.size() == 4 ? std::optional{a[3]} : std::nullopt);
    }
    if ((a.size() == 4 || a.size() == 5) && a[1] == "consolidate") {
        return cmd_consolidate(native(a[2]), native(a[3]),
                               a.size() == 5 ? std::optional{a[4]} : std::nullopt);
    }
    if (a.size() == 4 && (a[1] == "codec-probe" || a[1] == "codec-probe-classes")) {
        return cmd_codec_probe(native(a[2]), a[3],
                               a[1] == "codec-probe-classes" ? ProbeMode::classes : ProbeMode::whole);
    }
    if (a.size() == 5 && a[1] == "codec-probe-framed") {
        // Only the recipe frame steps (not "whole"), spelled as in the recipe text.
        for (std::size_t i = 1; i < recipe_frame_limits.size(); ++i) {
            if (a[4] == std::to_string(recipe_frame_limits[i] >> 20) + "MiB") {
                return cmd_codec_probe(native(a[2]), a[3], ProbeMode::framed, recipe_frame_limits[i]);
            }
        }
        std::cerr << "unknown frame limit: " << a[4] << "\n";
        return 2;
    }
    if (a.size() == 5 && a[1] == "dup-probe") {
        return cmd_dup_probe(native(a[2]), a[3], a[4]);
    }
    if (a.size() == 5 && a[1] == "cut-probe") {
        return cmd_cut_probe(native(a[2]), a[3], a[4]);
    }
    if (a.size() == 3 && a[1] == "describe") {
        return cmd_describe(native(a[2]));
    }
    if (a.size() == 6 && a[1] == "extract") {
        return cmd_extract(native(a[2]), a[3], native(a[4]), a[5]);
    }
    if (a.size() == 6 && a[1] == "extract-profile") {
        const auto count = number(a[4]);
        if (!count || *count == 0 || *count > 1000) {
            return 2;
        }
        return cmd_extract_profile(native(a[2]), native(a[3]), static_cast<std::size_t>(*count),
                                   native(a[5]));
    }
    if ((a.size() == 8 || a.size() == 9) && a[1] == "solid-search" &&
        (a[2] == "seed" || a[2] == "explicit")) {
        const bool seeded = a[2] == "seed";
        const auto bits = hex32(a[3]);
        const auto start = a[3] == "default" ? std::optional{Plan{}}
                           : seeded          ? (bits ? decode_solid_seed(*bits) : std::nullopt)
                                             : parse_solid_explicit(a[3]);
        const auto budget = number(a[6]), prng = number(a[7]);
        const std::string policy = a.size() == 9 ? std::string{a[8]} : "near";
        if (!start || !budget || !prng || (policy != "near" && policy != "enumerate")) {
            std::cerr << "invalid solid search arguments\n";
            return 2;
        }
        return cmd_solid_search(seeded, *start, native(a[4]), native(a[5]), *budget, *prng, policy);
    }
    if (a.size() == 4 && a[1] == "payload-compare") {
        return cmd_payload_compare(native(a[2]), native(a[3]));
    }
    if (a.size() == 3 && a[1] == "solid-selftest") {
        return cmd_solid_selftest(native(a[2]));
    }
    if (a.size() >= 6 && a.size() <= 8 && a[1] == "pack" &&
        (a[2] == "seed" || a[2] == "explicit" || a[2] == "solid" || a[2] == "seed-solid" ||
         a[2] == "explicit-solid")) {
        bool ok = true;
        const auto wanted = a.size() >= 7 ? redundancy_arg(a[6], ok) : std::nullopt;
        std::uint64_t cpb = default_checkpoint_bytes;
        if (a.size() == 8) {
            const auto n = number(a[7]);
            ok = ok && n && *n >= min_checkpoint_bytes && *n <= max_checkpoint_bytes;
            cpb = n.value_or(0);
        }
        if (!ok) {
            std::cerr << usage;
            return 2;
        }
        return cmd_pack(std::string{a[2]}, a[3], native(a[4]), native(a[5]), wanted,
                        static_cast<std::uint32_t>(cpb));
    }
    if (a.size() >= 5 && a.size() <= 8 && a[1] == "append") {
        std::string how = "segment";
        std::optional<Plan> plan;
        for (std::size_t i = 5; i < a.size(); ++i) {
            if (a[i] == "segment" || a[i] == "regroup") {
                how = std::string{a[i]};
                continue;
            }
            if (a[i] != "--plan" || i + 1 == a.size() || plan) {
                std::cerr << usage; // unknown token, missing value or a second --plan
                return 2;
            }
            plan = plan_arg(a[i + 1]);
            ++i;
            if (!plan) {
                std::cerr << usage;
                return 2;
            }
        }
        return cmd_append(native(a[2]), native(a[3]), native(a[4]), how, plan);
    }
    if (a.size() == 4 && a[1] == "verify") {
        return cmd_verify(native(a[2]), native(a[3]));
    }
    if (a.size() == 5 && a[1] == "peek") {
        const auto n = number(a[4]);
        if (!n || *n == 0 || *n > 10000) {
            std::cerr << usage;
            return 2;
        }
        return cmd_peek(native(a[2]), native(a[3]), static_cast<std::size_t>(*n));
    }
    if (a.size() == 4 && a[1] == "partial") {
        return cmd_partial(native(a[2]), native(a[3]));
    }
    if (a.size() == 3 && a[1] == "selftest") {
        return cmd_selftest(native(a[2]));
    }
    if ((a.size() == 5 || a.size() == 6) && a[1] == "split") {
        const auto n = number(a[3]);
        std::uint32_t mode = carry_gen;
        if (a.size() == 6) {
            const auto it = std::ranges::find(carry_names, a[5]);
            mode = it == carry_names.end() ? 99U
                                           : static_cast<std::uint32_t>(it - carry_names.begin());
        }
        if (!n || *n < min_volume_bytes || *n > max_logical || mode > carry_dicts) {
            std::cerr << usage;
            return 2;
        }
        return cmd_split(native(a[2]), *n, native(a[4]), mode);
    }
    if ((a.size() == 4 || a.size() == 6) && a[1] == "vappend") {
        std::optional<Plan> plan;
        if (a.size() == 6) {
            plan = a[4] == "--plan" ? plan_arg(a[5]) : std::nullopt;
            if (!plan) {
                std::cerr << usage;
                return 2;
            }
        }
        return cmd_vappend(native(a[2]), native(a[3]), plan);
    }
    if (a.size() == 4 && a[1] == "vverify") {
        return cmd_vverify(native(a[2]), native(a[3]));
    }
    if (a.size() == 4 && a[1] == "vpartial") {
        return cmd_vpartial(native(a[2]), native(a[3]));
    }
    if (a.size() >= 7 && a.size() <= 9 && a[1] == "search" && (a[2] == "seed" || a[2] == "coord")) {
        const auto start = a[3] == "default" ? std::optional{default_start()} : plan_arg(a[3]);
        const auto budget = number(a[5]);
        const auto prng = number(a[6]);
        const bool restart = a.size() < 8 || a[7] == "restart";
        const auto stall = a.size() == 9 ? number(a[8]) : std::optional<std::uint64_t>{20};
        if (!start || !budget || *budget == 0 || !prng ||
            (a.size() >= 8 && a[7] != "restart" && a[7] != "local") || !stall || *stall == 0 ||
            *stall > 10000) {
            std::cerr << usage;
            return 2;
        }
        return cmd_search(std::string{a[2]}, *start, native(a[4]), *budget, *prng, restart,
                          static_cast<int>(*stall));
    }
    std::cerr << usage;
    return 2;
}
