// SPDX-License-Identifier: GPL-3.0-or-later
// DEV-017 research tool: appending to an archive (research/ideas/seed-append-r1.md, 7.2-7.3).
// Writes EXPERIMENTAL research container v3 (not the SeedArc archive format; nothing frozen):
//
//   header (32) | generation 0: dictionaries, data | appended data | ... | trailer | footer (24)
//
// The trailer (plaintext, CRC32C-checked) holds the group table, the group-key table, either
// the Seed record (seed mode: groups are recomputed from paths and sizes) or one group byte per
// file (explicit mode), the generation history and the index with explicit data offsets. An
// append writes old bytes + new data + a new trailer, so the old container is a byte prefix of
// the new one; its old trailer stays in place and its bytes are counted. Solid mode (baseline)
// stores one zstd frame per group instead of per-block frames.
//
// Grouping is insertion-stable: at generation 0 the k-1 keys with the most bytes get their own
// group and are written to the key table; every other key, including keys first seen in later
// appends, goes to the "other" group. Plans (rule, k, per-group D / T) use the G5 Seed layout.
#include "append_common.hpp"

#include <cstring>
#include <iostream>

namespace {
using namespace seedarc::research_tools;
using namespace seedarc::research_tools::append;

constexpr std::string_view usage =
    "Research-only append study (container v3); not the SeedArc archive format.\n"
    "  append_study pack seed|explicit|solid PLAN_HEX INPUT_DIR OUT\n"
    "  append_study append OLD NEW_DIR OUT [segment|regroup]   (solid containers: how to append)\n"
    "  append_study verify CONTAINER INPUT_DIR                  (every file, byte for byte)\n"
    "  append_study peek CONTAINER INPUT_DIR N                  (N cold single-file extractions)\n"
    "  append_study selftest INPUT_DIR\n";
constexpr std::array<char, 8> magic{'S', 'A', 'D', 'X', 'R', '0', '0', '3'};
constexpr std::array<char, 8> footer_magic{'S', 'A', 'D', 'X', 'E', 'N', 'D', '3'};
constexpr std::uint32_t version = 3;
constexpr std::size_t header_bytes = 32;
constexpr std::size_t footer_bytes = 24;
constexpr std::uint32_t max_generations = 1024;
enum : std::uint32_t { flag_seed = 1, flag_solid = 2 };

Error bad(const char *what) {
    return {ErrorCode::integrity_error, std::string{"research container v3: "} + what};
}
int fail(const Error &e) {
    std::cerr << e.message << '\n';
    return 1;
}

// ---- the container model
// --------------------------------------------------------------------------
struct Entry {
    std::string path;
    std::uint64_t size = 0;
    std::uint8_t group = 0;
    std::vector<Block> blocks;      // block mode
    std::uint32_t frame = 0;        // solid mode
    std::uint64_t frame_offset = 0; // solid mode
    std::uint32_t file_crc = 0;     // solid mode
};
struct Group {
    std::uint64_t dict_offset = 0;
    std::uint32_t dict_size = 0;
    std::uint32_t dict_crc = 0;
};
struct Frame {
    std::uint64_t offset, stored, raw;
    std::uint8_t group;
};
struct Generation {
    std::uint32_t index, seed, files_after;
};
struct Model {
    std::uint32_t flags = 0;
    Plan plan;
    Grouping grouping;
    std::vector<Group> groups;
    std::vector<Entry> entries;
    std::vector<Frame> frames;
    std::vector<Generation> history;
    std::uint64_t trailer_offset = 0; // of the current trailer
    std::uint64_t data_end = 0;       // where the next generation's data starts (= trailer_offset)
};

// Group metadata bytes (what H-meta compares): key table plus Seed record, or per-file bytes.
std::uint64_t key_table_bytes(const Grouping &g) {
    std::uint64_t n = 4;
    for (const auto &[key, id] : g.keys) {
        n += 2 + key.size() + 1;
    }
    return n;
}

std::vector<std::byte> trailer_bytes(const Model &m) {
    std::vector<std::byte> t;
    put(t, m.entries.size(), 4);
    put(t, m.groups.size(), 4);
    for (const auto &g : m.groups) {
        put(t, g.dict_offset, 8);
        put(t, g.dict_size, 4);
        put(t, g.dict_crc, 4);
    }
    put(t, m.grouping.keys.size(), 4);
    for (const auto &[key, id] : m.grouping.keys) {
        put(t, key.size(), 2);
        for (const auto c : key) {
            t.push_back(static_cast<std::byte>(c));
        }
        t.push_back(static_cast<std::byte>(id));
    }
    if ((m.flags & flag_seed) != 0) {
        put(t, seed_revision, 2);
        put(t, 4, 2);
        put(t, encode_seed(m.plan), 4);
    } else if ((m.flags & flag_solid) == 0) {
        for (const auto &e : m.entries) {
            t.push_back(static_cast<std::byte>(e.group));
        }
    }
    put(t, m.history.size(), 4);
    for (const auto &h : m.history) {
        put(t, h.index, 4);
        put(t, h.seed, 4);
        put(t, h.files_after, 4);
    }
    for (const auto &e : m.entries) {
        put(t, e.path.size(), 2);
        for (const auto c : e.path) {
            t.push_back(static_cast<std::byte>(c));
        }
        put(t, e.size, 8);
        if ((m.flags & flag_solid) != 0) {
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
    if ((m.flags & flag_solid) != 0) {
        put(t, m.frames.size(), 4);
        for (const auto &f : m.frames) {
            put(t, f.offset, 8);
            put(t, f.stored, 8);
            put(t, f.raw, 8);
            t.push_back(static_cast<std::byte>(f.group));
        }
    }
    return t;
}

std::vector<std::byte> header(std::uint32_t flags) {
    std::vector<std::byte> h(magic.size());
    std::memcpy(h.data(), magic.data(), magic.size());
    put(h, version, 4);
    put(h, chunk, 4);
    put(h, level, 4);
    put(h, flags, 4);
    h.resize(header_bytes);
    return h;
}

// Appends the trailer and footer for `m` to `out` (m.trailer_offset must equal out.size()).
void finish_container(std::vector<std::byte> &out, const Model &m) {
    const auto t = trailer_bytes(m);
    out.insert(out.end(), t.begin(), t.end());
    put(out, m.trailer_offset, 8);
    put(out, t.size(), 4);
    put(out, seedarc::experimental::crc32c(t), 4);
    const auto at = out.size();
    out.resize(at + footer_magic.size());
    std::memcpy(out.data() + at, footer_magic.data(), footer_magic.size());
}

// Bounded, strict parse of the current generation (footer -> trailer).
std::expected<Model, Error> parse(std::span<const std::byte> bytes) {
    if (bytes.size() < header_bytes + footer_bytes ||
        std::memcmp(bytes.data(), magic.data(), magic.size()) != 0 ||
        std::memcmp(bytes.data() + bytes.size() - footer_magic.size(), footer_magic.data(),
                    footer_magic.size()) != 0) {
        return std::unexpected(bad("bad magic"));
    }
    Reader h{bytes, magic.size()};
    const auto ver = h.get(4), ck = h.get(4), lv = h.get(4), flags = h.get(4), r1 = h.get(4),
               r2 = h.get(4);
    if (!r2 || *ver != version || *ck != chunk || *lv != level || (*flags & ~3ULL) != 0 ||
        *flags == (flag_seed | flag_solid) || *r1 != 0 || *r2 != 0) {
        return std::unexpected(bad("bad header"));
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
    Model m;
    m.flags = static_cast<std::uint32_t>(*flags);
    m.trailer_offset = *toff;
    m.data_end = *toff;
    const bool solid = (m.flags & flag_solid) != 0;
    Reader t{trailer, 0};
    const auto count = t.get(4), groups = t.get(4);
    if (!groups || *count > max_files || *groups == 0 || *groups > max_groups) {
        return std::unexpected(bad("bad counts"));
    }
    for (std::uint64_t g = 0; g < *groups; ++g) {
        const auto off = t.get(8), size = t.get(4), crc = t.get(4);
        if (!crc || *size > research::max_dictionary_bytes || *off > *toff ||
            *toff - *off < *size || (*size != 0 && *off < header_bytes)) {
            return std::unexpected(bad("bad group table"));
        }
        const auto dict =
            bytes.subspan(static_cast<std::size_t>(*off), static_cast<std::size_t>(*size));
        if (seedarc::experimental::crc32c(dict) != *crc) {
            return std::unexpected(bad("dictionary checksum mismatch"));
        }
        m.groups.push_back(
            {*off, static_cast<std::uint32_t>(*size), static_cast<std::uint32_t>(*crc)});
    }
    const auto nkeys = t.get(4);
    if (!nkeys || *nkeys >= *groups) {
        return std::unexpected(bad("bad key table"));
    }
    std::set<std::uint8_t> ids;
    for (std::uint64_t i = 0; i < *nkeys; ++i) {
        const auto len = t.get(2);
        if (!len || *len > max_path || trailer.size() - t.at < *len + 1) {
            return std::unexpected(bad("bad key"));
        }
        std::string key(reinterpret_cast<const char *>(trailer.data() + t.at),
                        static_cast<std::size_t>(*len));
        t.at += static_cast<std::size_t>(*len);
        const auto id = static_cast<std::uint8_t>(*t.get(1));
        if (id >= *groups - 1 || !ids.insert(id).second ||
            !m.grouping.keys.emplace(key, id).second) {
            return std::unexpected(bad("bad key id"));
        }
    }
    m.grouping.other = static_cast<std::uint8_t>(*groups - 1);
    if (ids.size() != *groups - 1) {
        return std::unexpected(bad("key table does not name every group but the last"));
    }
    std::vector<std::uint8_t> explicit_groups;
    if ((m.flags & flag_seed) != 0) {
        const auto rev = t.get(2), len = t.get(2), seed = t.get(4);
        if (!seed || *rev != seed_revision || *len != 4) {
            return std::unexpected(bad("bad seed record"));
        }
        const auto plan = decode_seed(static_cast<std::uint32_t>(*seed));
        if (!plan || canonical(*plan) != *plan || *groups > plan->k) {
            return std::unexpected(bad("bad seed"));
        }
        m.plan = *plan;
    } else if (!solid) {
        if (trailer.size() - t.at < *count) {
            return std::unexpected(bad("truncated group bytes"));
        }
        for (std::uint64_t i = 0; i < *count; ++i) {
            explicit_groups.push_back(static_cast<std::uint8_t>(*t.get(1)));
            if (explicit_groups.back() >= *groups) {
                return std::unexpected(bad("bad file group"));
            }
        }
    }
    const auto gens = t.get(4);
    if (!gens || *gens == 0 || *gens > max_generations) {
        return std::unexpected(bad("bad history"));
    }
    for (std::uint64_t i = 0; i < *gens; ++i) {
        const auto idx = t.get(4), seed = t.get(4), after = t.get(4);
        if (!after || *idx != i || *after > *count ||
            (!m.history.empty() && *after <= m.history.back().files_after)) {
            return std::unexpected(bad("bad history entry"));
        }
        m.history.push_back({static_cast<std::uint32_t>(*idx), static_cast<std::uint32_t>(*seed),
                             static_cast<std::uint32_t>(*after)});
    }
    if (m.history.back().files_after != *count) {
        return std::unexpected(bad("history does not cover every file"));
    }
    std::set<std::string> paths;
    for (std::uint64_t i = 0; i < *count; ++i) {
        const auto len = t.get(2);
        if (!len || *len == 0 || *len > max_path || trailer.size() - t.at < *len) {
            return std::unexpected(bad("bad path"));
        }
        Entry e;
        e.path.assign(reinterpret_cast<const char *>(trailer.data() + t.at),
                      static_cast<std::size_t>(*len));
        t.at += static_cast<std::size_t>(*len);
        const auto size = t.get(8);
        if (!size || *size > max_input || !paths.insert(e.path).second) {
            return std::unexpected(bad("bad entry"));
        }
        e.size = *size;
        if (solid) {
            const auto frame = t.get(4), off = t.get(8), crc = t.get(4);
            if (!crc) {
                return std::unexpected(bad("bad solid entry"));
            }
            e.frame = static_cast<std::uint32_t>(*frame);
            e.frame_offset = *off;
            e.file_crc = static_cast<std::uint32_t>(*crc);
        } else {
            e.group = (m.flags & flag_seed) != 0 ? m.grouping.of(m.plan.rule, e.path, e.size)
                                                 : explicit_groups[static_cast<std::size_t>(i)];
            const bool has_dict = m.groups[e.group].dict_size != 0;
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
                    *off < header_bytes || *off > *toff || *toff - *off < *stored) {
                    return std::unexpected(bad("bad block"));
                }
                e.blocks.push_back({*off, static_cast<std::uint32_t>(*stored),
                                    static_cast<std::uint8_t>(*kind),
                                    static_cast<std::uint32_t>(*crc)});
            }
        }
        m.entries.push_back(std::move(e));
    }
    if (solid) {
        const auto nframes = t.get(4);
        if (!nframes || *nframes > max_files) {
            return std::unexpected(bad("bad frame count"));
        }
        for (std::uint64_t i = 0; i < *nframes; ++i) {
            const auto off = t.get(8), stored = t.get(8), raw = t.get(8), group = t.get(1);
            if (!group || *group >= *groups || *off < header_bytes || *off > *toff ||
                *toff - *off < *stored || *stored == 0 || *raw == 0 ||
                *raw > research::max_solid_bytes) {
                return std::unexpected(bad("bad frame"));
            }
            m.frames.push_back({*off, *stored, *raw, static_cast<std::uint8_t>(*group)});
        }
        for (auto &e : m.entries) {
            if (e.frame >= m.frames.size() || e.frame_offset > m.frames[e.frame].raw ||
                m.frames[e.frame].raw - e.frame_offset < e.size) {
                return std::unexpected(bad("solid entry outside its frame"));
            }
            e.group = m.frames[e.frame].group;
        }
    }
    if (t.at != trailer.size()) {
        return std::unexpected(bad("trailing bytes in the trailer"));
    }
    return m;
}

// One solid frame over `members` (in order), appended to `out`; fills the entries' positions.
std::expected<void, Error> encode_solid(const std::vector<const InputFile *> &members,
                                        std::uint8_t group, std::vector<Entry *> entries, Model &m,
                                        std::vector<std::byte> &out) {
    std::vector<std::byte> joined;
    for (std::size_t i = 0; i < members.size(); ++i) {
        entries[i]->frame = static_cast<std::uint32_t>(m.frames.size());
        entries[i]->frame_offset = joined.size();
        entries[i]->file_crc = seedarc::experimental::crc32c(members[i]->data);
        joined.insert(joined.end(), members[i]->data.begin(), members[i]->data.end());
    }
    if (joined.empty()) {
        // An all-empty group still needs a frame for its entries: store one zero byte.
        joined.push_back(std::byte{0});
    }
    auto frame = research::compress_solid(joined, level);
    if (!frame) {
        return std::unexpected(frame.error());
    }
    m.frames.push_back({out.size(), frame->size(), joined.size(), group});
    out.insert(out.end(), frame->begin(), frame->end());
    return {};
}

std::vector<std::vector<const InputFile *>>
members_by_group(const Model &m, const std::vector<InputFile> &files,
                 const std::vector<std::uint8_t> &groups) {
    std::vector<std::vector<const InputFile *>> by(m.groups.size());
    for (std::size_t i = 0; i < files.size(); ++i) {
        by[groups[i]].push_back(&files[i]);
    }
    return by;
}

// ---- decoding -----------------------------------------------------------------------------------
std::expected<std::map<std::string, std::vector<std::byte>>, Error>
decode_all(const Model &m, std::span<const std::byte> bytes) {
    std::map<std::string, std::vector<std::byte>> out;
    if ((m.flags & flag_solid) != 0) {
        std::vector<std::vector<std::byte>> raw(m.frames.size());
        for (std::size_t i = 0; i < m.frames.size(); ++i) {
            raw[i].resize(static_cast<std::size_t>(m.frames[i].raw));
            const auto frame = bytes.subspan(static_cast<std::size_t>(m.frames[i].offset),
                                             static_cast<std::size_t>(m.frames[i].stored));
            if (auto r = research::decompress_solid(frame, raw[i]); !r) {
                return std::unexpected(r.error());
            }
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
    std::vector<std::optional<research::DictionaryDecoder>> dicts;
    for (const auto &g : m.groups) {
        if (g.dict_size == 0) {
            dicts.emplace_back();
            continue;
        }
        auto created = research::DictionaryDecoder::create(
            bytes.subspan(static_cast<std::size_t>(g.dict_offset), g.dict_size));
        if (!created) {
            return std::unexpected(created.error());
        }
        dicts.emplace_back(std::move(*created));
    }
    codec::Decoder plain;
    for (const auto &e : m.entries) {
        std::vector<std::byte> file(static_cast<std::size_t>(e.size));
        std::size_t at = 0;
        for (const auto &b : e.blocks) {
            const auto raw = std::min<std::size_t>(chunk, file.size() - at);
            const auto in = bytes.subspan(static_cast<std::size_t>(b.offset), b.stored);
            const auto dst = std::span{file}.subspan(at, raw);
            if (b.kind == kind_store) {
                std::ranges::copy(in, dst.begin());
            } else if (b.kind == kind_zstd) {
                if (auto r = plain.decode(codec::zstd_id, in, dst); !r) {
                    return std::unexpected(r.error());
                }
            } else if (auto r = dicts[e.group]->decode(in, dst); !r) {
                return std::unexpected(r.error());
            }
            if (seedarc::experimental::crc32c(dst) != b.crc) {
                return std::unexpected(bad("block crc mismatch"));
            }
            at += raw;
        }
        out.emplace(e.path, std::move(file));
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

std::string parts_json(const Model &m, std::span<const std::byte> bytes) {
    std::uint64_t dicts = 0, data = 0;
    for (const auto &g : m.groups) {
        dicts += g.dict_size;
    }
    for (const auto &e : m.entries) {
        for (const auto &b : e.blocks) {
            data += b.stored;
        }
    }
    for (const auto &f : m.frames) {
        data += f.stored;
    }
    const auto trailer = bytes.size() - footer_bytes - m.trailer_offset;
    const auto stale = m.trailer_offset - header_bytes - dicts - data; // superseded trailers
    const bool seed = (m.flags & flag_seed) != 0;
    const bool solid = (m.flags & flag_solid) != 0;
    const auto group_meta = key_table_bytes(m.grouping) + (seed ? 8 : solid ? 0 : m.entries.size());
    return "\"container_bytes\":" + std::to_string(bytes.size()) +
           ",\"parts\":{\"header\":" + std::to_string(header_bytes) +
           ",\"dictionaries\":" + std::to_string(dicts) + ",\"data\":" + std::to_string(data) +
           ",\"stale_trailers\":" + std::to_string(stale) +
           ",\"trailer\":" + std::to_string(trailer) +
           ",\"footer\":" + std::to_string(footer_bytes) +
           ",\"group_metadata\":" + std::to_string(group_meta) +
           "},\"files\":" + std::to_string(m.entries.size()) +
           ",\"groups\":" + std::to_string(m.groups.size()) +
           ",\"generations\":" + std::to_string(m.history.size());
}

// ---- modes --------------------------------------------------------------------------------------
std::expected<std::vector<std::byte>, Error> pack_bytes(const std::string &mode, const Plan &plan,
                                                        const std::vector<InputFile> &files) {
    Model m;
    m.flags = mode == "seed"    ? std::uint32_t{flag_seed}
              : mode == "solid" ? std::uint32_t{flag_solid}
                                : 0U;
    m.plan = plan;
    m.grouping = initial_grouping(plan, files);
    m.groups.resize(m.grouping.groups());
    std::vector<std::uint8_t> group_of;
    for (const auto &f : files) {
        group_of.push_back(m.grouping.of(plan.rule, f.path, f.data.size()));
    }
    auto out = header(m.flags);
    const auto by = members_by_group(m, files, group_of);
    std::vector<std::vector<std::byte>> dicts(m.groups.size());
    if ((m.flags & flag_solid) == 0) {
        for (std::size_t g = 0; g < m.groups.size(); ++g) {
            if (dict_sizes[plan.d[g]] != 0 && !by[g].empty()) {
                auto d = train(by[g], plan.d[g], plan.t[g]);
                if (!d) {
                    return std::unexpected(d.error());
                }
                dicts[g] = std::move(*d);
            }
            m.groups[g] = {dicts[g].empty() ? 0 : out.size(),
                           static_cast<std::uint32_t>(dicts[g].size()),
                           seedarc::experimental::crc32c(dicts[g])};
            out.insert(out.end(), dicts[g].begin(), dicts[g].end());
        }
    }
    m.entries.resize(files.size());
    for (std::size_t i = 0; i < files.size(); ++i) {
        m.entries[i].path = files[i].path;
        m.entries[i].size = files[i].data.size();
        m.entries[i].group = group_of[i];
    }
    if ((m.flags & flag_solid) != 0) {
        for (std::size_t g = 0; g < m.groups.size(); ++g) {
            if (by[g].empty()) {
                continue;
            }
            std::vector<Entry *> es;
            for (std::size_t i = 0; i < files.size(); ++i) {
                if (group_of[i] == g) {
                    es.push_back(&m.entries[i]);
                }
            }
            if (auto r = encode_solid(by[g], static_cast<std::uint8_t>(g), es, m, out); !r) {
                return std::unexpected(r.error());
            }
        }
    } else {
        for (std::size_t i = 0; i < files.size(); ++i) {
            auto blocks = encode_blocks(files[i], dicts[group_of[i]], out);
            if (!blocks) {
                return std::unexpected(blocks.error());
            }
            m.entries[i].blocks = std::move(*blocks);
        }
    }
    m.history.push_back({0, (m.flags & flag_seed) != 0 ? encode_seed(plan) : 0,
                         static_cast<std::uint32_t>(files.size())});
    m.trailer_offset = out.size();
    finish_container(out, m);
    return out;
}

// Explicit mode's D(b) heuristic: the group of the existing file sharing the longest path prefix.
std::uint8_t nearest_group(const Model &m, std::string_view path) {
    std::size_t best_len = 0;
    std::uint8_t best = m.grouping.other;
    for (const auto &e : m.entries) {
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
                                                          const std::string &solid_mode) {
    auto parsed = parse(old);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    Model m = std::move(*parsed);
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
    const bool solid = (m.flags & flag_solid) != 0;
    std::vector<std::uint8_t> group_of;
    for (const auto &f : added) {
        group_of.push_back((m.flags & flag_seed) != 0
                               ? m.grouping.of(m.plan.rule, f.path, f.data.size())
                           : solid ? m.grouping.of(0, f.path, f.data.size()) // replaced below
                                   : nearest_group(m, f.path));
    }
    if (solid) {
        // Solid containers keep the grouping of their entries' frames: a new file joins the group
        // of the existing file with the longest common path prefix (no rule is stored).
        for (std::size_t i = 0; i < added.size(); ++i) {
            group_of[i] = nearest_group(m, added[i].path);
        }
    }
    const auto old_count = m.entries.size();
    for (std::size_t i = 0; i < added.size(); ++i) {
        Entry e;
        e.path = added[i].path;
        e.size = added[i].data.size();
        e.group = group_of[i];
        m.entries.push_back(std::move(e));
    }
    std::vector<std::byte> out;
    if (solid && solid_mode == "regroup") {
        // Re-compress every group that receives a new file as one new frame (old + new files);
        // untouched frames are copied. The result is a fresh container, not an extension.
        auto decoded = decode_all(
            Model{m.flags, m.plan, m.grouping, m.groups,
                  std::vector<Entry>{m.entries.begin(),
                                     m.entries.begin() + static_cast<std::ptrdiff_t>(old_count)},
                  m.frames, m.history, m.trailer_offset, m.data_end},
            old);
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        std::vector<InputFile> all;
        for (std::size_t i = 0; i < old_count; ++i) {
            all.push_back({m.entries[i].path, (*decoded)[m.entries[i].path]});
        }
        for (const auto &f : added) {
            all.push_back(f);
        }
        std::set<std::uint8_t> touched(group_of.begin(), group_of.end());
        Model n{m.flags, m.plan, m.grouping, m.groups, {}, {}, m.history, 0, 0};
        out = header(n.flags);
        n.entries.resize(all.size());
        for (std::size_t i = 0; i < all.size(); ++i) {
            n.entries[i].path = all[i].path;
            n.entries[i].size = all[i].data.size();
            n.entries[i].group = m.entries[i].group;
        }
        for (std::size_t g = 0; g < n.groups.size(); ++g) {
            std::vector<const InputFile *> members;
            std::vector<Entry *> es;
            for (std::size_t i = 0; i < all.size(); ++i) {
                if (n.entries[i].group == g) {
                    members.push_back(&all[i]);
                    es.push_back(&n.entries[i]);
                }
            }
            if (members.empty()) {
                continue;
            }
            if (!touched.contains(static_cast<std::uint8_t>(g))) {
                // Copy each old frame of this group unchanged.
                std::map<std::uint32_t, std::uint32_t> remap;
                for (std::size_t i = 0; i < old_count; ++i) {
                    if (m.entries[i].group != g) {
                        continue;
                    }
                    const auto oldf = m.entries[i].frame;
                    if (!remap.contains(oldf)) {
                        const auto &fr = m.frames[oldf];
                        remap[oldf] = static_cast<std::uint32_t>(n.frames.size());
                        n.frames.push_back({out.size(), fr.stored, fr.raw, fr.group});
                        const auto src = old.subspan(static_cast<std::size_t>(fr.offset),
                                                     static_cast<std::size_t>(fr.stored));
                        out.insert(out.end(), src.begin(), src.end());
                    }
                    n.entries[i].frame = remap[oldf];
                    n.entries[i].frame_offset = m.entries[i].frame_offset;
                    n.entries[i].file_crc = m.entries[i].file_crc;
                }
                continue;
            }
            if (auto r = encode_solid(members, static_cast<std::uint8_t>(g), es, n, out); !r) {
                return std::unexpected(r.error());
            }
        }
        n.history.push_back({static_cast<std::uint32_t>(n.history.size()), 0,
                             static_cast<std::uint32_t>(all.size())});
        n.trailer_offset = out.size();
        finish_container(out, n);
        return out;
    }
    // Extension: old bytes stay a byte prefix; new data and a new trailer follow.
    out.assign(old.begin(), old.end());
    if (solid) {
        for (std::size_t g = 0; g < m.groups.size(); ++g) {
            std::vector<const InputFile *> members;
            std::vector<Entry *> es;
            for (std::size_t i = 0; i < added.size(); ++i) {
                if (group_of[i] == g) {
                    members.push_back(&added[i]);
                    es.push_back(&m.entries[old_count + i]);
                }
            }
            if (!members.empty()) {
                if (auto r = encode_solid(members, static_cast<std::uint8_t>(g), es, m, out); !r) {
                    return std::unexpected(r.error());
                }
            }
        }
    } else {
        for (std::size_t i = 0; i < added.size(); ++i) {
            const auto &g = m.groups[group_of[i]];
            const auto dict = old.subspan(static_cast<std::size_t>(g.dict_offset), g.dict_size);
            auto blocks = encode_blocks(added[i], dict, out);
            if (!blocks) {
                return std::unexpected(blocks.error());
            }
            m.entries[old_count + i].blocks = std::move(*blocks);
        }
    }
    m.history.push_back({static_cast<std::uint32_t>(m.history.size()),
                         (m.flags & flag_seed) != 0 ? encode_seed(m.plan) : 0,
                         static_cast<std::uint32_t>(m.entries.size())});
    m.trailer_offset = out.size();
    finish_container(out, m);
    return out;
}

int cmd_pack(const std::string &mode, std::string_view plan_hex, const fs::path &input,
             const fs::path &out_path) {
    const auto seed = hex32(plan_hex);
    const auto plan = seed ? decode_seed(*seed) : std::nullopt;
    if (!plan || canonical(*plan) != *plan) {
        std::cerr << "not a canonical plan\n";
        return 2;
    }
    const auto w = wall_ms(), c = cpu_ms();
    auto files = read_tree(input);
    if (!files) {
        return fail(files.error());
    }
    const auto rw = wall_ms(), rc = cpu_ms();
    auto bytes = pack_bytes(mode, *plan, *files);
    if (!bytes) {
        return fail(bytes.error());
    }
    const auto pack_wall = wall_ms() - rw, pack_cpu = cpu_ms() - rc;
    if (auto r = write_new(out_path, *bytes); !r) {
        return fail(r.error());
    }
    auto m = parse(*bytes);
    if (!m) {
        return fail(m.error());
    }
    if (auto r = byte_exact(*files, *m, *bytes); !r) {
        return fail(r.error());
    }
    std::uint64_t input_bytes = 0;
    for (const auto &f : *files) {
        input_bytes += f.data.size();
    }
    std::cout << "{\"schema\":\"seedarc.append-study.r1\",\"mode\":\"pack\",\"metadata\":"
              << json_string(mode) << ",\"plan_seed\":" << *seed << ",\"rule\":\""
              << rule_names[plan->rule] << "\",\"k\":" << static_cast<int>(plan->k)
              << ",\"input_bytes\":" << input_bytes << "," << parts_json(*m, *bytes)
              << ",\"pack_wall_ms\":" << pack_wall << ",\"pack_cpu_ms\":" << pack_cpu
              << ",\"call_wall_ms\":" << wall_ms() - w << ",\"call_cpu_ms\":" << cpu_ms() - c
              << ",\"byte_exact\":true," << peak_memory_json() << "}\n";
    return std::cout ? 0 : 1;
}

int cmd_append(const fs::path &old_path, const fs::path &new_dir, const fs::path &out_path,
               const std::string &solid_mode) {
    const auto w = wall_ms(), c = cpu_ms();
    auto old = read_file(old_path);
    if (!old) {
        return fail(old.error());
    }
    auto added = read_tree(new_dir);
    if (!added) {
        return fail(added.error());
    }
    std::uint64_t added_bytes = 0;
    for (const auto &f : *added) {
        added_bytes += f.data.size();
    }
    const auto aw = wall_ms(), ac = cpu_ms();
    auto bytes = append_bytes(*old, *added, solid_mode);
    if (!bytes) {
        return fail(bytes.error());
    }
    const auto append_wall = wall_ms() - aw, append_cpu = cpu_ms() - ac;
    if (auto r = write_new(out_path, *bytes); !r) {
        return fail(r.error());
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
    const bool prefix =
        bytes->size() >= old->size() && std::equal(old->begin(), old->end(), bytes->begin());
    std::cout << "{\"schema\":\"seedarc.append-study.r1\",\"mode\":\"append\",\"solid_mode\":"
              << json_string(solid_mode) << ",\"appended_files\":" << added->size()
              << ",\"appended_bytes\":" << added_bytes << ",\"old_container_bytes\":" << old->size()
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
    std::cout << "{\"schema\":\"seedarc.append-study.r1\",\"mode\":\"verify\",\"byte_exact\":true,"
                 "\"files\":"
              << files->size() << ",\"generations\":" << m->history.size()
              << ",\"decode_wall_ms\":" << decode_wall << ",\"decode_cpu_ms\":" << decode_cpu << ","
              << peak_memory_json() << "}\n";
    return std::cout ? 0 : 1;
}

// Cold single-file extraction: read the container, parse, decode only what that file needs
// (its blocks, or its whole solid frame).
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
        Model one = *m;
        one.entries = {*it};
        if ((one.flags & flag_solid) != 0) {
            one.frames = {m->frames[it->frame]};
            one.entries[0].frame = 0;
        }
        auto decoded = decode_all(one, *bytes);
        if (!decoded) {
            return fail(decoded.error());
        }
        times.push_back(wall_ms() - w);
        if (decoded->at(want.path) != want.data) {
            return fail(bad("single-file extraction mismatch"));
        }
    }
    std::ranges::sort(times);
    const auto q = [&](double p) {
        return times[std::min(
            times.size() - 1,
            static_cast<std::size_t>(p * static_cast<double>(times.size() - 1) + 0.5))];
    };
    std::cout << "{\"schema\":\"seedarc.append-study.r1\",\"mode\":\"peek\",\"byte_exact\":true,"
                 "\"samples\":"
              << times.size() << ",\"median_ms\":" << q(0.5) << ",\"p90_ms\":" << q(0.9)
              << ",\"max_ms\":" << times.back() << "," << peak_memory_json() << "}\n";
    return std::cout ? 0 : 1;
}

// Damage, prefix and recompute self-test on a real input split into two generations.
int cmd_selftest(const fs::path &input) {
    auto files = read_tree(input);
    if (!files || files->size() < 4) {
        return fail(files ? bad("selftest needs at least 4 files") : files.error());
    }
    std::vector<InputFile> first(
        files->begin(), files->begin() + static_cast<std::ptrdiff_t>(files->size() * 3 / 4));
    std::vector<InputFile> second(
        files->begin() + static_cast<std::ptrdiff_t>(files->size() * 3 / 4), files->end());
    Plan plan;
    plan.rule = 0;
    plan.k = 3;
    plan.d = {2, 1, 0};
    plan.t = {1, 0, 0};
    plan = canonical(plan);
    std::uint64_t tried = 0, rejected = 0, checks = 0, stale_checked = 0;
    for (const auto *mode : {"seed", "explicit", "solid"}) {
        for (const auto *how : {"segment", "regroup"}) {
            if (std::string{mode} != "solid" && std::string{how} == "regroup") {
                continue;
            }
            auto packed = pack_bytes(mode, plan, first);
            if (!packed) {
                return fail(packed.error());
            }
            auto grown = append_bytes(*packed, second, how);
            if (!grown) {
                return fail(grown.error());
            }
            auto m = parse(*grown);
            if (!m || !byte_exact(*files, *m, *grown) || m->history.size() != 2) {
                return fail(bad("selftest: grown container does not restore every file"));
            }
            const bool prefix = std::equal(packed->begin(), packed->end(), grown->begin());
            if (prefix != (std::string{how} == "segment")) {
                return fail(bad("selftest: prefix property"));
            }
            // A second identical append is refused (duplicate paths).
            if (append_bytes(*grown, second, how)) {
                return fail(bad("selftest: duplicate append accepted"));
            }
            checks += 3;
            const auto &good = *grown;
            // The superseded trailer of the first generation is never read again: damage there
            // must leave the container restoring correctly, damage anywhere else must be refused.
            std::size_t stale_begin = good.size(), stale_end = good.size();
            if (prefix) {
                stale_begin = static_cast<std::size_t>(parse(*packed)->trailer_offset);
                stale_end = packed->size();
            }
            std::vector<std::size_t> offsets{0,
                                             8,
                                             12,
                                             16,
                                             20,
                                             24,
                                             header_bytes,
                                             good.size() / 2,
                                             good.size() - 1,
                                             good.size() - 9,
                                             good.size() - 17};
            for (std::size_t at = 33; at < good.size(); at += good.size() / 40 + 1) {
                offsets.push_back(at);
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
                        return fail(
                            bad("selftest: damage in a superseded trailer broke the container"));
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
        }
    }
    std::cout << "{\"schema\":\"seedarc.append-study.r1\",\"mode\":\"selftest\",\"checks\":"
              << checks << ",\"damaged_tried\":" << tried << ",\"damaged_rejected\":" << rejected
              << ",\"superseded_trailer_bytes_checked_inert\":" << stale_checked << "}\n";
    return rejected == tried ? 0 : 1;
}
} // namespace

int main(int argc, char *argv[]) {
    const std::vector<std::string_view> a(argv, argv + argc);
    if (a.size() == 6 && a[1] == "pack" &&
        (a[2] == "seed" || a[2] == "explicit" || a[2] == "solid")) {
        return cmd_pack(std::string{a[2]}, a[3], native(a[4]), native(a[5]));
    }
    if ((a.size() == 5 || a.size() == 6) && a[1] == "append") {
        const std::string how = a.size() == 6 ? std::string{a[5]} : "segment";
        if (how != "segment" && how != "regroup") {
            std::cerr << usage;
            return 2;
        }
        return cmd_append(native(a[2]), native(a[3]), native(a[4]), how);
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
    if (a.size() == 3 && a[1] == "selftest") {
        return cmd_selftest(native(a[2]));
    }
    std::cerr << usage;
    return 2;
}
