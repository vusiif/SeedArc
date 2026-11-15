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
//   file        a FIL record in front of every file
// Solid mode writes a FRM record in front of every frame unless the level is off.
//
// Records: tag (8) | body length (4) | body | CRC32C(body) (4). Every body starts with the
// record's own offset, so a record is only valid where it claims to be (copies inside stored
// data or old trailers are ignored). The trailer copies every GEN body and lists every file
// with explicit offsets; reading the whole container cross-checks the trailer against the
// records and fails closed on any difference. Recovery from a truncated or damaged file uses
// the newest intact trailer in the prefix plus every valid record, verifies every file by CRC,
// refuses conflicting copies and applies the production manifest checks (ADR-0005).
#include "append_common.hpp"

#include <cstring>
#include <iostream>

namespace {
using namespace seedarc::research_tools;
using namespace seedarc::research_tools::append;

constexpr std::string_view usage =
    "Research-only append study (container v4); not the SeedArc archive format.\n"
    "  append4_study pack seed|explicit|solid PLAN_HEX INPUT_DIR OUT [auto|off|checkpoint|file]"
    " [CHECKPOINT_BYTES]\n"
    "  append4_study append OLD NEW_DIR OUT [segment|regroup] [--plan PLAN_HEX]\n"
    "  append4_study verify CONTAINER INPUT_DIR      (every file, byte for byte)\n"
    "  append4_study peek CONTAINER INPUT_DIR N      (N cold single-file extractions)\n"
    "  append4_study partial CONTAINER INPUT_DIR     (recovery from truncated prefixes)\n"
    "  append4_study selftest INPUT_DIR\n";
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
std::span<const std::byte> as_bytes(const Tag &t) {
    return std::as_bytes(std::span{t});
}
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
    bool seed() const { return (flags & flag_seed) != 0; }
    bool solid() const { return (flags & flag_solid) != 0; }
    bool explicit_groups() const { return !seed() && !solid(); }
};
std::vector<std::byte> header_of(const Header &h) {
    std::vector<std::byte> out(magic.size());
    std::memcpy(out.data(), magic.data(), magic.size());
    put(out, version, 4);
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
    if (!cpb || *ver != version || *ck != chunk || *lv != level || (*flags & ~3ULL) != 0 ||
        *flags == (flag_seed | flag_solid) || *red > red_file ||
        (*red == red_checkpoint) != (*cpb != 0) ||
        (*cpb != 0 && (*cpb < min_checkpoint_bytes || *cpb > max_checkpoint_bytes))) {
        return std::unexpected(bad("bad header"));
    }
    return Header{static_cast<std::uint32_t>(*flags), static_cast<std::uint32_t>(*red),
                  static_cast<std::uint32_t>(*cpb)};
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
    Plan plan;
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
std::optional<Record> read_record(std::span<const std::byte> bytes, std::size_t at) {
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
    if (c.get(4) != seedarc::experimental::crc32c(body) || self.get(8) != at) {
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
    if (g.has_seed) {
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
    if (h.seed()) {
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
    // Distinct ids with the largest equal to nkeys - 1 are exactly 0 .. nkeys - 1.
    if (!ngroups || *ngroups == 0 || *ngroups > max_groups || *nkeys != *ngroups - 1 ||
        (!ids.empty() && *ids.rbegin() != *nkeys - 1) || (g.has_seed && *ngroups > g.plan.k)) {
        return std::unexpected(bad("key table does not name every group but the last"));
    }
    g.grouping.other = static_cast<std::uint8_t>(*ngroups - 1);
    for (std::uint64_t i = 0; i < *ngroups; ++i) {
        const auto off = r.get(8), size = r.get(4), crc = r.get(4);
        if (!crc || *size > research::max_dictionary_bytes ||
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
std::expected<Scan, Error> scan(std::span<const std::byte> bytes, const Header &h, bool decode) {
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
                *raw > research::max_solid_bytes || *stored == 0 || *nfiles > max_files) {
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
            const auto avail = std::min<std::uint64_t>(*stored, bytes.size() - r->end);
            s.frames.push_back({r->end, *stored, *raw, g.index, static_cast<std::uint8_t>(*grp)});
            s.frame_raw.emplace_back();
            s.frame_produced.push_back(0);
            if (decode && avail > 0) {
                auto &out = s.frame_raw.back();
                out.resize(static_cast<std::size_t>(*raw));
                const auto in = bytes.subspan(r->end, static_cast<std::size_t>(avail));
                if (avail == *stored) {
                    if (research::decompress_solid(in, out)) {
                        s.frame_produced.back() = out.size();
                    }
                } else if (auto n = research::decompress_solid_prefix(in, out)) {
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
    put(t, seedarc::experimental::crc32c(header_of(m.header)), 4); // the header has no CRC of its own
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

// Bounded, strict parse of the whole container (footer -> trailer -> records), cross-checked
// against a scan of the records; any difference is an error.
std::expected<Model, Error> parse(std::span<const std::byte> bytes) {
    auto header = parse_header(bytes);
    if (!header) {
        return std::unexpected(header.error());
    }
    if (bytes.size() < header_bytes + footer_bytes ||
        std::memcmp(bytes.data() + bytes.size() - footer_magic.size(), footer_magic.data(),
                    footer_magic.size()) != 0) {
        return std::unexpected(bad("bad footer magic"));
    }
    Reader f{bytes, bytes.size() - footer_bytes};
    const auto toff = f.get(8), tsize = f.get(4), tcrc = f.get(4);
    const auto footer_at = bytes.size() - footer_bytes;
    if (!tcrc || *toff < header_bytes || *toff > footer_at || footer_at - *toff != *tsize) {
        return std::unexpected(bad("bad footer"));
    }
    const auto trailer =
        bytes.subspan(static_cast<std::size_t>(*toff), static_cast<std::size_t>(*tsize));
    if (seedarc::experimental::crc32c(trailer) != *tcrc) {
        return std::unexpected(bad("trailer checksum mismatch"));
    }
    const auto body_bytes = bytes.first(static_cast<std::size_t>(*toff));
    Model m;
    m.header = *header;
    m.trailer_offset = *toff;
    const auto &h = m.header;
    Reader t{trailer, 0};
    const auto hcrc = t.get(4), count = t.get(4), ngens = t.get(4);
    if (!ngens || *hcrc != seedarc::experimental::crc32c(bytes.first(header_bytes)) ||
        *count > max_files || *ngens == 0 || *ngens > max_generations) {
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
        const auto rec = read_record(body_bytes, static_cast<std::size_t>(*off));
        if (!rec || rec->type != rec_gen || !std::ranges::equal(rec->body, copy)) {
            return std::unexpected(bad("generation record differs from its trailer copy"));
        }
        auto g = parse_gen_body(copy, h);
        if (!g) {
            return std::unexpected(g.error());
        }
        if (g->index != i || g->first_file != prev_after || *after <= prev_after ||
            *after > *count ||
            (!m.gens.empty() && *off < m.data_start.back())) {
            return std::unexpected(bad("bad generation order"));
        }
        const auto ds = check_gen_dicts(body_bytes, *g, rec->end, &m.gens);
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
                *off < m.data_start[*fgen] || *off > region_end(static_cast<std::uint32_t>(*fgen)) ||
                region_end(static_cast<std::uint32_t>(*fgen)) - *off < *stored || *stored == 0 ||
                *raw == 0 || *raw > research::max_solid_bytes) {
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
        }
    }
    if (t.at != trailer.size()) {
        return std::unexpected(bad("trailing bytes in the trailer"));
    }
    if (auto ok = check_manifest(paths); !ok) {
        return std::unexpected(ok.error());
    }
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
    return m;
}

// ---- decoding -----------------------------------------------------------------------------------
// Dictionary decoders by (offset, size): one per distinct dictionary in the container.
using DecoderCache = std::map<std::pair<std::uint64_t, std::uint32_t>, research::DictionaryDecoder>;

std::expected<std::vector<std::byte>, Error> decode_blocks(const Entry &e, const GenGroup &grp,
                                                           std::span<const std::byte> bytes,
                                                           DecoderCache &cache) {
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
    std::vector<std::byte> file(static_cast<std::size_t>(e.size));
    std::size_t at = 0;
    for (const auto &b : e.blocks) {
        const auto raw = std::min<std::size_t>(chunk, file.size() - at);
        if (b.offset > bytes.size() || bytes.size() - b.offset < b.stored) {
            return std::unexpected(bad("block outside the container"));
        }
        const auto in = bytes.subspan(static_cast<std::size_t>(b.offset), b.stored);
        const auto dst = std::span{file}.subspan(at, raw);
        if (b.kind == kind_store) {
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
        at += raw;
    }
    return file;
}

std::expected<std::vector<std::byte>, Error> decode_frame(const Frame &f,
                                                          std::span<const std::byte> bytes) {
    std::vector<std::byte> raw(static_cast<std::size_t>(f.raw));
    const auto frame =
        bytes.subspan(static_cast<std::size_t>(f.offset), static_cast<std::size_t>(f.stored));
    if (auto r = research::decompress_solid(frame, raw); !r) {
        return std::unexpected(r.error());
    }
    return raw;
}

std::expected<std::map<std::string, std::vector<std::byte>>, Error>
decode_all(const Model &m, std::span<const std::byte> bytes) {
    std::map<std::string, std::vector<std::byte>> out;
    if (m.header.solid()) {
        std::vector<std::vector<std::byte>> raw;
        for (const auto &f : m.frames) {
            auto r = decode_frame(f, bytes);
            if (!r) {
                return std::unexpected(r.error());
            }
            raw.push_back(std::move(*r));
        }
        for (const auto &e : m.entries) {
            const auto view = std::span{raw[e.frame]}.subspan(
                static_cast<std::size_t>(e.frame_offset), static_cast<std::size_t>(e.size));
            if (seedarc::experimental::crc32c(view) != e.file_crc) {
                return std::unexpected(bad("file crc mismatch"));
            }
            out.emplace(e.path, std::vector<std::byte>{view.begin(), view.end()});
        }
        return out;
    }
    DecoderCache cache;
    for (const auto &e : m.entries) {
        auto file = decode_blocks(e, m.gens[e.gen].groups[e.group], bytes, cache);
        if (!file) {
            return std::unexpected(file.error());
        }
        out.emplace(e.path, std::move(*file));
    }
    return out;
}

std::expected<void, Error> byte_exact(const std::vector<InputFile> &files, const Model &m,
                                      std::span<const std::byte> bytes) {
    auto decoded = decode_all(m, bytes);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    if (decoded->size() != files.size()) {
        return std::unexpected(bad("file count differs from input"));
    }
    for (const auto &f : files) {
        const auto it = decoded->find(f.path);
        if (it == decoded->end() || it->second != f.data) {
            return std::unexpected(bad("round trip mismatch"));
        }
    }
    return {};
}

// ---- recovery from a prefix or a damaged file ---------------------------------------------------
struct Recovery {
    bool header_ok = false;
    std::map<std::string, std::vector<std::byte>> files;
    std::uint64_t from_trailer = 0, from_records = 0, unreadable = 0;
};
std::expected<Recovery, Error> recover(std::span<const std::byte> bytes) {
    Recovery out;
    auto header = parse_header(bytes);
    if (!header) {
        return out; // not even the header: nothing can be read
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
    for (auto it = bytes.begin(); (it = std::search(it, bytes.end(), fm.begin(), fm.end())) !=
                                  bytes.end();
         ++it) {
        footers.push_back(static_cast<std::size_t>(it - bytes.begin()));
    }
    for (auto f = footers.rbegin(); f != footers.rend(); ++f) {
        auto m = parse(bytes.first(*f + footer_magic.size()));
        if (!m) {
            continue;
        }
        DecoderCache cache;
        std::vector<std::optional<std::vector<std::byte>>> frames(m->frames.size());
        for (const auto &e : m->entries) {
            std::expected<std::vector<std::byte>, Error> data = std::unexpected(bad("unread"));
            if (m->header.solid()) {
                auto &raw = frames[e.frame];
                if (!raw) {
                    auto r = decode_frame(m->frames[e.frame], bytes);
                    raw = r ? std::move(*r) : std::vector<std::byte>{};
                }
                const auto view = std::span{*raw};
                if (view.size() >= e.frame_offset + e.size) {
                    const auto cut = view.subspan(static_cast<std::size_t>(e.frame_offset),
                                                  static_cast<std::size_t>(e.size));
                    if (seedarc::experimental::crc32c(cut) == e.file_crc) {
                        data = std::vector<std::byte>{cut.begin(), cut.end()};
                    }
                }
            } else {
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
    auto s = scan(bytes, *header, true);
    if (!s) {
        return std::unexpected(s.error());
    }
    DecoderCache cache;
    for (const auto &[index, e] : s->entries) {
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
        } else {
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
    for (const auto &[path, data] : out.files) {
        paths.push_back(path);
    }
    if (auto ok = check_manifest(paths); !ok) {
        return std::unexpected(ok.error());
    }
    return out;
}

// ---- encoding and layout ------------------------------------------------------------------------
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
                                          const std::vector<std::span<const std::byte>> &dicts) {
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
    for (std::size_t g = 0; g < p.gen.groups.size(); ++g) {
        std::vector<std::byte> joined;
        bool any = false;
        for (std::size_t i = 0; i < files.size(); ++i) {
            if (groups[i] != g) {
                continue;
            }
            any = true;
            auto &e = p.entries[i];
            e.frame = static_cast<std::uint32_t>(p.frames.size());
            e.frame_offset = joined.size();
            e.file_crc = seedarc::experimental::crc32c(files[i].data);
            joined.insert(joined.end(), files[i].data.begin(), files[i].data.end());
        }
        if (!any) {
            continue;
        }
        if (joined.empty()) {
            joined.push_back(std::byte{0}); // an all-empty group still needs a frame
        }
        if (joined.size() > research::max_solid_bytes) {
            return std::unexpected(Error{ErrorCode::resource_limit, "solid group too large"});
        }
        auto frame = research::compress_solid(joined, level);
        if (!frame) {
            return std::unexpected(frame.error());
        }
        p.frames.push_back({0, frame->size(), joined.size(), 0, static_cast<std::uint8_t>(g)});
        p.payloads.push_back(std::move(*frame));
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
                    return std::unexpected(Error{ErrorCode::resource_limit, "file record too large"});
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
    h.flags = mode == "seed" ? std::uint32_t{flag_seed} : mode == "solid" ? std::uint32_t{flag_solid} : 0U;
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
                                        std::optional<std::uint32_t> wanted, std::uint32_t cpb) {
    std::vector<std::uint8_t> groups;
    const auto base = header_for(mode, red_off, cpb);
    auto p = fresh_pending(base, plan, files, groups);
    if (!p) {
        return std::unexpected(p.error());
    }
    if (auto r = encode_pending(*p, base, files, groups, dict_views(*p, {})); !r) {
        return std::unexpected(r.error());
    }
    Packed out;
    std::array<std::vector<std::byte>, 3> all;
    for (std::uint32_t red = red_off; red <= red_file; ++red) {
        if (wanted && *wanted != red) {
            continue;
        }
        auto bytes = lay_out_fresh(header_for(mode, red, cpb), *p);
        if (!bytes) {
            return std::unexpected(bytes.error());
        }
        out.candidate_bytes[red] = bytes->size();
        all[red] = std::move(*bytes);
    }
    if (wanted) {
        out.redundancy = *wanted;
    } else {
        out.recommended = true;
        const auto off = static_cast<double>(out.candidate_bytes[red_off]);
        const auto within = [&](std::uint32_t red) {
            return static_cast<double>(out.candidate_bytes[red]) - off <= rec_overhead_budget * off;
        };
        out.redundancy = out.candidate_bytes[red_off] < rec_small_archive ? red_off
                         : within(red_file)                               ? red_file
                         : within(red_checkpoint)                         ? red_checkpoint
                                                                          : red_off;
    }
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
        if (g.groups != ref.groups || !(g.grouping == ref.grouping)) {
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

std::expected<std::vector<std::byte>, Error> append_bytes(std::span<const std::byte> old,
                                                          const std::vector<InputFile> &added,
                                                          const std::string &how,
                                                          const std::optional<Plan> &plan) {
    auto parsed = parse(old);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    Model m = std::move(*parsed);
    const auto &h = m.header;
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
            if (!(g.grouping == m.gens[0].grouping)) {
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
            all.push_back({e.path, (*decoded)[e.path]});
            groups.push_back(e.group);
        }
        for (const auto &f : added) {
            groups.push_back(nearest_group(m, last, f.path));
            all.push_back(f);
        }
        Pending p;
        p.gen = m.gens[0];
        p.new_dicts.resize(p.gen.groups.size());
        if (auto r = encode_pending(p, h, all, groups, dict_views(p, old)); !r) {
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
    } else {
        // Variant A: keep the previous generation's Seed, key table and dictionaries.
        p.gen = last;
        p.new_dicts.resize(p.gen.groups.size());
        for (const auto &f : added) {
            groups.push_back(h.seed() ? group_of(last, f.path, f.data.size())
                                      : nearest_group(m, last, f.path));
        }
    }
    if (auto r = encode_pending(p, h, added, groups, dict_views(p, old)); !r) {
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
        for (const auto &[key, id] : g.grouping.keys) {
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
    const auto redundancy = m.record_bytes[rec_file] + m.record_bytes[rec_checkpoint] +
                            m.record_bytes[rec_frame];
    const auto trailer = bytes.size() - footer_bytes - m.trailer_offset;
    const auto stale = m.trailer_offset - header_bytes - gen_records - dicts - data - redundancy;
    // Group metadata (H-meta): key tables plus the Seed records, or per-file group bytes (in the
    // trailer, and uncompressed in FIL records when the level is "file").
    const auto per_file = m.header.explicit_groups()
                              ? m.entries.size() * (m.header.redundancy == red_file ? 2U : 1U)
                              : 0U;
    const auto group_meta = key_tables + (m.header.seed() ? 8 * m.gens.size() : 0) + per_file;
    return "\"container_bytes\":" + std::to_string(bytes.size()) +
           ",\"redundancy\":\"" + redundancy_names[m.header.redundancy] +
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

// ---- commands -----------------------------------------------------------------------------------
int cmd_pack(const std::string &mode, std::string_view plan_hex, const fs::path &input,
             const fs::path &out_path, std::optional<std::uint32_t> wanted, std::uint32_t cpb) {
    const auto plan = plan_arg(plan_hex);
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
    auto packed = pack_bytes(mode, *plan, *files, wanted, cpb);
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
        candidates += std::string{red == 0 ? "" : ","} + "\"" + redundancy_names[red] + "\":" +
                      (packed->candidate_bytes[red] != 0
                           ? std::to_string(packed->candidate_bytes[red])
                           : std::string{"null"});
    }
    std::cout << "{\"schema\":\"seedarc.append-study.v4.r1\",\"mode\":\"pack\",\"metadata\":"
              << json_string(mode) << ",\"plan_seed\":" << encode_seed(*plan) << ",\"rule\":\""
              << rule_names[plan->rule] << "\",\"k\":" << static_cast<int>(plan->k)
              << ",\"input_bytes\":" << input_bytes_of(*files) << "," << parts_json(*m, packed->bytes)
              << ",\"redundancy_chosen_by\":\""
              << (packed->recommended ? "recommendation" : "user")
              << "\",\"recommendation_rule\":\"rec-r0 (provisional)\",\"candidate_bytes\":{"
              << candidates << "},\"pack_wall_ms\":" << pack_wall << ",\"pack_cpu_ms\":" << pack_cpu
              << ",\"call_wall_ms\":" << wall_ms() - w << ",\"call_cpu_ms\":" << cpu_ms() - c
              << ",\"byte_exact\":true," << peak_memory_json() << "}\n";
    return std::cout ? 0 : 1;
}

int cmd_append(const fs::path &old_path, const fs::path &new_dir, const fs::path &out_path,
               const std::string &how, const std::optional<Plan> &plan) {
    const auto w = wall_ms(), c = cpu_ms();
    auto old = read_file(old_path);
    if (!old) {
        return fail(old.error());
    }
    auto added = read_tree(new_dir);
    if (!added) {
        return fail(added.error());
    }
    const auto aw = wall_ms(), ac = cpu_ms();
    auto bytes = append_bytes(*old, *added, how, plan);
    if (!bytes) {
        return fail(bytes.error());
    }
    const auto append_wall = wall_ms() - aw, append_cpu = cpu_ms() - ac;
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
              << parts_json(*m, *bytes) << ",\"append_wall_ms\":" << append_wall
              << ",\"append_cpu_ms\":" << append_cpu << ",\"call_wall_ms\":" << wall_ms() - w
              << ",\"call_cpu_ms\":" << cpu_ms() - c << ",\"appended_byte_exact\":true,"
              << peak_memory_json() << "}\n";
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
    auto decoded = decode_all(*m, *bytes);
    if (!decoded) {
        return fail(decoded.error());
    }
    const auto decode_wall = wall_ms() - w, decode_cpu = cpu_ms() - c;
    auto files = read_tree(input);
    if (!files) {
        return fail(files.error());
    }
    if (files->size() != decoded->size()) {
        return fail(bad("file count differs from input"));
    }
    for (const auto &f : *files) {
        const auto it = decoded->find(f.path);
        if (it == decoded->end() || it->second != f.data) {
            return fail(bad("round trip mismatch"));
        }
    }
    std::cout << "{\"schema\":\"seedarc.append-study.v4.r1\",\"mode\":\"verify\",\"byte_exact\":"
                 "true,\"files\":"
              << files->size() << ",\"generations\":" << m->gens.size() << ",\"redundancy\":\""
              << redundancy_names[m->header.redundancy] << "\",\"decode_wall_ms\":" << decode_wall
              << ",\"decode_cpu_ms\":" << decode_cpu << "," << peak_memory_json() << "}\n";
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
            auto raw = decode_frame(m->frames[it->frame], *bytes);
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
std::expected<CutResult, Error> recover_checked(std::span<const std::byte> bytes,
                                                const std::map<std::string_view,
                                                               const InputFile *> &by_path) {
    auto r = recover(bytes);
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
    std::cout << "{\"schema\":\"seedarc.append-study.v4.r1\",\"mode\":\"partial\",\"container_bytes\":"
              << bytes->size() << ",\"input_files\":" << files->size()
              << ",\"input_bytes\":" << total << ",\"cuts\":[";
    std::vector<std::uint64_t> cuts;
    for (int pct = 5; pct <= 95; pct += 5) {
        cuts.push_back(bytes->size() * static_cast<std::uint64_t>(pct) / 100);
    }
    cuts.push_back(bytes->size() - 1);
    cuts.push_back(bytes->size());
    for (std::size_t i = 0; i < cuts.size(); ++i) {
        const auto w = wall_ms();
        auto r = recover_checked(std::span{*bytes}.first(static_cast<std::size_t>(cuts[i])), by_path);
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
    std::uint64_t checks = 0, tried = 0, rejected = 0, stale_checked = 0, cut_checks = 0;
    const std::uint32_t cpb = min_checkpoint_bytes;
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
                std::vector<std::size_t> offsets{0, 8, 12, 16, 20, 24, 28, header_bytes,
                                                 good.size() / 2, good.size() - 1,
                                                 good.size() - 9, good.size() - 17};
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
                    const auto keep = pct == 100 ? good.size() - 1
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
    std::cout << "{\"schema\":\"seedarc.append-study.v4.r1\",\"mode\":\"selftest\",\"checks\":"
              << checks << ",\"damaged_tried\":" << tried << ",\"damaged_rejected\":" << rejected
              << ",\"superseded_trailer_bytes_checked_inert\":" << stale_checked
              << ",\"recovery_checks\":" << cut_checks << "}\n";
    return rejected == tried ? 0 : 1;
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
} // namespace

int main(int argc, char *argv[]) {
    const std::vector<std::string_view> a(argv, argv + argc);
    if (a.size() >= 6 && a.size() <= 8 && a[1] == "pack" &&
        (a[2] == "seed" || a[2] == "explicit" || a[2] == "solid")) {
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
            } else if (a[i] == "--plan" && i + 1 < a.size() && (plan = plan_arg(a[i + 1]))) {
                ++i;
            } else {
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
    std::cerr << usage;
    return 2;
}
