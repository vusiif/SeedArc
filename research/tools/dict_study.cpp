// SPDX-License-Identifier: GPL-3.0-or-later
// DEV-013 research tool: shared zstd dictionary feasibility. Writes an EXPERIMENTAL research
// container (not the SeedArc archive format; nothing here is frozen) whose every byte (header,
// dictionary, group table, index, data) counts toward the reported size. Verification and
// single-file extraction decode into memory and compare with the input; nothing is extracted to
// disk. Portable: Windows uses GetProcessTimes / PeakWorkingSetSize, other systems getrusage.
#include "codec/codec.hpp"
#include "codec/dictionary.hpp"
#include "seedarc/archive.hpp"
#include "seedarc/archive_index.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <psapi.h>
#else
#include <sys/resource.h>
#endif

namespace fs = std::filesystem;
namespace codec = seedarc::experimental::codec;
namespace research = seedarc::experimental::codec::research;
using seedarc::Error;
using seedarc::ErrorCode;

namespace {
constexpr std::string_view usage =
    "Research-only shared-dictionary study; not the SeedArc archive format.\n"
    "  dict_study pack INPUT_DIR OUT LEVEL DICT_BYTES TRAIN_BYTES   (DICT_BYTES 0: no dictionary;\n"
    "                                                               TRAIN_BYTES 0: all input)\n"
    "  dict_study verify CONTAINER INPUT_DIR\n"
    "  dict_study peek CONTAINER INPUT_DIR N          (N single-file extractions, cold each)\n"
    "  dict_study peek-sarc ARCHIVE INPUT_DIR N       (same, for a production .sarc archive)\n";
constexpr std::array<char, 8> magic{'S', 'A', 'D', 'X', 'R', '0', '0', '1'};
constexpr std::uint32_t version = 1;
constexpr std::uint32_t chunk = 65536;
constexpr std::size_t header_bytes = 32;
constexpr std::uint32_t max_files = 200000;
constexpr std::size_t max_path = 4096;
constexpr std::uint64_t max_input = 1ULL << 30;
enum : std::uint8_t { kind_store = 0, kind_zstd = 1, kind_zstd_dict = 2 };

// ---- measurement -------------------------------------------------------------------------
double wall_ms() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
double cpu_ms() {
#ifdef _WIN32
    FILETIME c{}, e{}, k{}, u{};
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) {
        return -1;
    }
    const auto ticks = [](const FILETIME &t) {
        return (static_cast<std::uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
    };
    return static_cast<double>(ticks(k) + ticks(u)) / 10000.0;
#else
    rusage r{};
    if (getrusage(RUSAGE_SELF, &r) != 0) {
        return -1;
    }
    return (r.ru_utime.tv_sec + r.ru_stime.tv_sec) * 1000.0 +
           (r.ru_utime.tv_usec + r.ru_stime.tv_usec) / 1000.0;
#endif
}
std::string peak_memory_json() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS m{};
    m.cb = sizeof(m);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &m, sizeof(m))) {
        return "\"peak_memory_bytes\":" + std::to_string(m.PeakWorkingSetSize) +
               ",\"peak_memory_metric\":\"PeakWorkingSetSize\"";
    }
#else
    rusage r{};
    if (getrusage(RUSAGE_SELF, &r) == 0) {
        return "\"peak_memory_bytes\":" +
               std::to_string(static_cast<std::uint64_t>(r.ru_maxrss) * 1024) +
               ",\"peak_memory_metric\":\"ru_maxrss\"";
    }
#endif
    return "\"peak_memory_bytes\":null,\"peak_memory_metric\":null";
}
std::string json_string(std::string_view value) {
    std::string out = "\"";
    for (const auto raw : value) {
        const auto c = static_cast<unsigned char>(raw);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += raw;
        } else if (c < 32) {
            constexpr char hex[] = "0123456789abcdef";
            out += "\\u00";
            out += hex[c >> 4];
            out += hex[c & 15];
        } else {
            out += raw;
        }
    }
    return out + "\"";
}

// ---- input --------------------------------------------------------------------------------
struct InputFile {
    std::string path; // UTF-8, '/' separated, relative
    std::vector<std::byte> data;
};
fs::path native(std::string_view utf8) { return fs::path{std::u8string{utf8.begin(), utf8.end()}}; }
std::string utf8(const fs::path &p) {
    const auto u = p.generic_u8string();
    return {u.begin(), u.end()};
}
std::expected<std::vector<std::byte>, Error> read_file(const fs::path &path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return std::unexpected(Error{ErrorCode::io_error, "cannot open " + utf8(path)});
    }
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec || size > max_input) {
        return std::unexpected(Error{ErrorCode::resource_limit, "input file too large"});
    }
    std::vector<std::byte> data(static_cast<std::size_t>(size));
    if (!stream.read(reinterpret_cast<char *>(data.data()),
                     static_cast<std::streamsize>(data.size()))) {
        return std::unexpected(Error{ErrorCode::io_error, "cannot read " + utf8(path)});
    }
    return data;
}
std::expected<std::vector<InputFile>, Error> read_tree(const fs::path &root) {
    std::vector<InputFile> files;
    std::uint64_t total = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec)) {
        if (ec) {
            return std::unexpected(Error{ErrorCode::io_error, "cannot enumerate input"});
        }
        if (it->is_symlink() || !it->is_regular_file()) {
            continue;
        }
        auto data = read_file(it->path());
        if (!data) {
            return std::unexpected(data.error());
        }
        total += data->size();
        if (files.size() == max_files || total > max_input) {
            return std::unexpected(Error{ErrorCode::resource_limit, "input too large"});
        }
        files.push_back({utf8(fs::relative(it->path(), root)), std::move(*data)});
    }
    std::ranges::sort(files, {}, &InputFile::path);
    return files;
}
std::uint64_t fnv1a(std::string_view text) {
    std::uint64_t h = 1469598103934665603ULL;
    for (const auto c : text) {
        h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ULL;
    }
    return h;
}

// ---- container ------------------------------------------------------------------------------
void put(std::vector<std::byte> &out, std::uint64_t value, int bytes) {
    for (int i = 0; i < bytes; ++i) {
        out.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xff));
    }
}
struct Reader {
    std::span<const std::byte> bytes;
    std::size_t at = 0;
    std::optional<std::uint64_t> get(int n) {
        if (bytes.size() - at < static_cast<std::size_t>(n)) {
            return std::nullopt;
        }
        std::uint64_t v = 0;
        for (int i = 0; i < n; ++i) {
            v |= std::uint64_t{std::to_integer<unsigned>(bytes[at + i])} << (8 * i);
        }
        at += static_cast<std::size_t>(n);
        return v;
    }
};
struct Block {
    std::uint32_t raw;
    std::uint32_t stored;
    std::uint8_t kind;
    std::uint32_t crc;
    std::uint64_t offset;
};
struct Entry {
    std::string path;
    std::uint64_t size;
    std::uint8_t group;
    std::vector<Block> blocks;
};
struct Container {
    std::uint32_t level = 0;
    std::vector<std::byte> dictionary;
    std::vector<Entry> entries;
    std::size_t data_start = 0;
    std::uint64_t data_bytes = 0;
};
Error bad(const char *what) {
    return {ErrorCode::integrity_error, std::string{"research container: "} + what};
}

// Bounded, strict parse of the research container (header, dictionary, groups, index).
std::expected<Container, Error> parse(std::span<const std::byte> bytes) {
    if (bytes.size() < header_bytes || std::memcmp(bytes.data(), magic.data(), magic.size()) != 0) {
        return std::unexpected(bad("bad magic"));
    }
    Reader r{bytes, magic.size()};
    const auto ver = r.get(4), ck = r.get(4), level = r.get(4), dict = r.get(4), count = r.get(4),
               index = r.get(4);
    if (!index || *ver != version || *ck != chunk || *level < 1 || *level > 19 ||
        *dict > research::max_dictionary_bytes || *count > max_files || *index > bytes.size()) {
        return std::unexpected(bad("bad header"));
    }
    Container c;
    c.level = static_cast<std::uint32_t>(*level);
    r.at = header_bytes;
    if (bytes.size() - r.at < *dict + *count + *index) {
        return std::unexpected(bad("truncated"));
    }
    c.dictionary.assign(bytes.begin() + static_cast<std::ptrdiff_t>(r.at),
                        bytes.begin() + static_cast<std::ptrdiff_t>(r.at + *dict));
    r.at += static_cast<std::size_t>(*dict);
    std::vector<std::uint8_t> groups(static_cast<std::size_t>(*count));
    for (auto &g : groups) {
        g = static_cast<std::uint8_t>(*r.get(1));
        if (g > 1 || (g == 1 && c.dictionary.empty())) {
            return std::unexpected(bad("bad group"));
        }
    }
    const auto index_end = r.at + static_cast<std::size_t>(*index);
    Reader ix{bytes.first(index_end), r.at};
    std::uint64_t offset = 0;
    for (std::uint32_t i = 0; i < *count; ++i) {
        const auto length = ix.get(2);
        if (!length || *length == 0 || *length > max_path || index_end - ix.at < *length) {
            return std::unexpected(bad("bad path"));
        }
        Entry e;
        e.path.assign(reinterpret_cast<const char *>(bytes.data() + ix.at),
                      static_cast<std::size_t>(*length));
        ix.at += static_cast<std::size_t>(*length);
        const auto size = ix.get(8), blocks = ix.get(4);
        if (!blocks || *size > max_input || *blocks != (*size + chunk - 1) / chunk) {
            return std::unexpected(bad("bad entry"));
        }
        e.size = *size;
        e.group = groups[i];
        for (std::uint64_t b = 0; b < *blocks; ++b) {
            const auto stored = ix.get(4), kind = ix.get(1), crc = ix.get(4);
            const auto raw =
                static_cast<std::uint32_t>(std::min<std::uint64_t>(chunk, *size - b * chunk));
            if (!crc || *kind > kind_zstd_dict || *stored == 0 || *stored > raw ||
                (*kind == kind_store) != (*stored == raw) ||
                (*kind == kind_zstd_dict && e.group != 1) || (*kind == kind_zstd && e.group != 0)) {
                return std::unexpected(bad("bad block"));
            }
            e.blocks.push_back({raw, static_cast<std::uint32_t>(*stored),
                                static_cast<std::uint8_t>(*kind), static_cast<std::uint32_t>(*crc),
                                offset});
            offset += *stored;
        }
        c.entries.push_back(std::move(e));
    }
    if (ix.at != index_end || bytes.size() - index_end != offset) {
        return std::unexpected(bad("index / data length mismatch"));
    }
    c.data_start = index_end;
    c.data_bytes = offset;
    return c;
}

struct Decoders {
    codec::Decoder plain;
    std::optional<research::DictionaryDecoder> dict;
};
std::expected<void, Error> decode_entry(const Container &c, Decoders &d, const Entry &e,
                                        std::span<const std::byte> bytes,
                                        std::vector<std::byte> &out) {
    out.resize(static_cast<std::size_t>(e.size));
    std::size_t at = 0;
    for (const auto &b : e.blocks) {
        const auto in = bytes.subspan(c.data_start + static_cast<std::size_t>(b.offset), b.stored);
        const auto dst = std::span{out}.subspan(at, b.raw);
        if (b.kind == kind_store) {
            std::ranges::copy(in, dst.begin());
        } else if (b.kind == kind_zstd) {
            if (auto r = d.plain.decode(codec::zstd_id, in, dst); !r) {
                return r;
            }
        } else {
            if (auto r = d.dict->decode(in, dst); !r) {
                return r;
            }
        }
        if (seedarc::experimental::crc32c(dst) != b.crc) {
            return std::unexpected(bad("crc mismatch"));
        }
        at += b.raw;
    }
    return {};
}

int fail(const Error &e) {
    std::cerr << e.message << '\n';
    return 1;
}

// ---- modes ----------------------------------------------------------------------------------
int pack(const fs::path &input, const fs::path &out_path, std::uint32_t level,
         std::size_t dict_bytes, std::uint64_t train_bytes) {
    const auto call_wall = wall_ms(), call_cpu = cpu_ms();
    auto files = read_tree(input);
    if (!files) {
        return fail(files.error());
    }
    const auto read_wall = wall_ms() - call_wall;
    // Training samples: each file's first block, in a fixed path-hash order, up to the budget.
    std::vector<std::byte> dictionary;
    std::string dict_status = dict_bytes == 0 ? "none" : "trained";
    double train_wall = 0, train_cpu = 0;
    std::uint64_t used = 0;
    std::size_t sample_count = 0;
    if (dict_bytes != 0) {
        const auto w = wall_ms(), cp = cpu_ms();
        std::vector<const InputFile *> order;
        for (const auto &f : *files) {
            if (!f.data.empty()) {
                order.push_back(&f);
            }
        }
        std::ranges::sort(order, {}, [](const InputFile *f) { return fnv1a(f->path); });
        std::vector<std::byte> samples;
        std::vector<std::size_t> sizes;
        for (const auto *f : order) {
            const auto n = std::min<std::size_t>(f->data.size(), chunk);
            if ((train_bytes != 0 && used + n > train_bytes) ||
                samples.size() + n > research::max_training_bytes) {
                break;
            }
            samples.insert(samples.end(), f->data.begin(),
                           f->data.begin() + static_cast<std::ptrdiff_t>(n));
            sizes.push_back(n);
            used += n;
        }
        sample_count = sizes.size();
        auto trained = research::train_dictionary(samples, sizes, dict_bytes);
        train_wall = wall_ms() - w;
        train_cpu = cpu_ms() - cp;
        if (trained) {
            dictionary = std::move(*trained);
        } else {
            dict_status =
                "training_failed: " + trained.error().message; // falls back to no dictionary
        }
    }
    const auto enc_wall = wall_ms(), enc_cpu = cpu_ms();
    auto plain = codec::Encoder::create(codec::zstd_id, level);
    std::optional<research::DictionaryEncoder> dict_encoder;
    if (!dictionary.empty()) {
        auto created = research::DictionaryEncoder::create(level, dictionary);
        if (!created) {
            return fail(created.error());
        }
        dict_encoder.emplace(std::move(*created));
    }
    if (!plain) {
        return fail(plain.error());
    }
    std::vector<std::byte> index, data, groups, packed(chunk);
    std::vector<std::uint64_t> file_stored;
    for (const auto &f : *files) {
        if (f.path.size() > max_path) {
            return fail(Error{ErrorCode::invalid_argument, "path too long"});
        }
        const std::uint8_t group = dict_encoder ? 1 : 0;
        groups.push_back(static_cast<std::byte>(group));
        put(index, f.path.size(), 2);
        for (const auto c : f.path) {
            index.push_back(static_cast<std::byte>(c));
        }
        put(index, f.data.size(), 8);
        const auto blocks = (f.data.size() + chunk - 1) / chunk;
        put(index, blocks, 4);
        std::uint64_t stored_total = 0;
        for (std::size_t b = 0; b < blocks; ++b) {
            const auto raw = std::span{f.data}.subspan(
                b * chunk, std::min<std::size_t>(chunk, f.data.size() - b * chunk));
            const auto room = std::span{packed}.first(raw.size() - 1);
            auto size = dict_encoder ? dict_encoder->encode(raw, room) : plain->encode(raw, room);
            if (!size) {
                return fail(size.error());
            }
            std::uint8_t kind =
                *size != 0 ? (dict_encoder ? kind_zstd_dict : kind_zstd) : kind_store;
            const auto stored = *size != 0 ? std::span<const std::byte>{packed}.first(*size) : raw;
            put(index, stored.size(), 4);
            index.push_back(static_cast<std::byte>(kind));
            put(index, seedarc::experimental::crc32c(raw), 4);
            data.insert(data.end(), stored.begin(), stored.end());
            stored_total += stored.size();
        }
        file_stored.push_back(stored_total);
    }
    std::vector<std::byte> out(magic.size());
    std::memcpy(out.data(), magic.data(), magic.size());
    put(out, version, 4);
    put(out, chunk, 4);
    put(out, level, 4);
    put(out, dictionary.size(), 4);
    put(out, files->size(), 4);
    put(out, index.size(), 4);
    out.resize(header_bytes);
    out.insert(out.end(), dictionary.begin(), dictionary.end());
    out.insert(out.end(), groups.begin(), groups.end());
    out.insert(out.end(), index.begin(), index.end());
    out.insert(out.end(), data.begin(), data.end());
    const auto encode_wall = wall_ms() - enc_wall, encode_cpu = cpu_ms() - enc_cpu;
    if (fs::exists(out_path)) {
        return fail(Error{ErrorCode::output_conflict, "output exists"});
    }
    std::ofstream stream(out_path, std::ios::binary);
    if (!stream.write(reinterpret_cast<const char *>(out.data()),
                      static_cast<std::streamsize>(out.size())) ||
        !stream.flush()) {
        return fail(Error{ErrorCode::io_error, "write failed"});
    }
    stream.close();
    // Self-check before reporting: the container must parse and restore every file.
    auto parsed = parse(out);
    if (!parsed || parsed->entries.size() != files->size()) {
        return fail(parsed ? bad("self-check entry count") : parsed.error());
    }
    std::uint64_t input_bytes = 0;
    for (const auto &f : *files) {
        input_bytes += f.data.size();
    }
    std::cout << "{\"schema\":\"seedarc.dict-study.r1\",\"mode\":\"pack\",\"level\":" << level
              << ",\"requested_dict_bytes\":" << dict_bytes
              << ",\"train_budget_bytes\":" << train_bytes
              << ",\"dictionary\":" << json_string(dict_status)
              << ",\"dict_id\":" << research::dictionary_id(dictionary)
              << ",\"training_samples\":" << sample_count << ",\"training_input_bytes\":" << used
              << ",\"files\":" << files->size() << ",\"input_bytes\":" << input_bytes
              << ",\"container_bytes\":" << out.size() << ",\"parts\":{\"header\":" << header_bytes
              << ",\"dictionary\":" << dictionary.size() << ",\"group_table\":" << groups.size()
              << ",\"index\":" << index.size() << ",\"data\":" << data.size()
              << "},\"read_wall_ms\":" << read_wall << ",\"train_wall_ms\":" << train_wall
              << ",\"train_cpu_ms\":" << train_cpu << ",\"encode_wall_ms\":" << encode_wall
              << ",\"encode_cpu_ms\":" << encode_cpu
              << ",\"call_wall_ms\":" << wall_ms() - call_wall
              << ",\"call_cpu_ms\":" << cpu_ms() - call_cpu << ",\"per_file\":[";
    for (std::size_t i = 0; i < files->size(); ++i) {
        std::cout << (i ? "," : "") << "[" << json_string((*files)[i].path) << ","
                  << (*files)[i].data.size() << "," << file_stored[i] << "]";
    }
    std::cout << "]," << peak_memory_json() << "}\n";
    return std::cout ? 0 : 1;
}

std::expected<Decoders, Error> make_decoders(const Container &c) {
    Decoders d;
    if (!c.dictionary.empty()) {
        auto created = research::DictionaryDecoder::create(c.dictionary);
        if (!created) {
            return std::unexpected(created.error());
        }
        d.dict.emplace(std::move(*created));
    }
    return d;
}

int verify(const fs::path &container_path, const fs::path &input) {
    const auto w = wall_ms(), cp = cpu_ms();
    auto bytes = read_file(container_path);
    if (!bytes) {
        return fail(bytes.error());
    }
    auto c = parse(*bytes);
    if (!c) {
        return fail(c.error());
    }
    auto d = make_decoders(*c);
    if (!d) {
        return fail(d.error());
    }
    std::vector<std::vector<std::byte>> decoded(c->entries.size());
    for (std::size_t i = 0; i < c->entries.size(); ++i) {
        if (auto r = decode_entry(*c, *d, c->entries[i], *bytes, decoded[i]); !r) {
            return fail(r.error());
        }
    }
    const auto decode_wall = wall_ms() - w, decode_cpu = cpu_ms() - cp;
    auto originals = read_tree(input);
    if (!originals || originals->size() != c->entries.size()) {
        return fail(originals ? bad("file count differs from input") : originals.error());
    }
    for (std::size_t i = 0; i < c->entries.size(); ++i) {
        if ((*originals)[i].path != c->entries[i].path || (*originals)[i].data != decoded[i]) {
            return fail(bad("round trip mismatch"));
        }
    }
    std::cout << "{\"schema\":\"seedarc.dict-study.r1\",\"mode\":\"verify\",\"byte_exact\":true,"
                 "\"files\":"
              << c->entries.size() << ",\"decode_wall_ms\":" << decode_wall
              << ",\"decode_cpu_ms\":" << decode_cpu << "," << peak_memory_json() << "}\n";
    return std::cout ? 0 : 1;
}

// Production archive single-file read: header + index + that file's blocks (codec::Decoder).
std::expected<std::vector<std::byte>, Error> sarc_one(const fs::path &archive,
                                                      const std::string &path) {
    auto bytes = read_file(archive);
    if (!bytes || bytes->size() < seedarc::experimental::archive_header_bytes) {
        return std::unexpected(bytes ? bad("short archive") : bytes.error());
    }
    const auto header = seedarc::experimental::decode_header(
        std::span{*bytes}.first(seedarc::experimental::archive_header_bytes));
    if (!header) {
        return std::unexpected(header.error());
    }
    const auto start = seedarc::experimental::archive_header_bytes + header->data_bytes;
    if (start > bytes->size()) {
        return std::unexpected(bad("bad data size"));
    }
    const auto index = seedarc::experimental::parse_index(
        *header, std::span{*bytes}.subspan(static_cast<std::size_t>(start)));
    if (!index) {
        return std::unexpected(index.error());
    }
    for (const auto &e : index->entries) {
        if (e.path != path || e.directory) {
            continue;
        }
        std::vector<std::byte> out(static_cast<std::size_t>(e.size));
        codec::Decoder decoder;
        std::size_t at = 0;
        for (std::uint64_t b = 0; b < e.block_count; ++b) {
            const auto &d = index->blocks[static_cast<std::size_t>(e.first_block + b)];
            const auto in = std::span{*bytes}.subspan(
                static_cast<std::size_t>(seedarc::experimental::archive_header_bytes +
                                         d.data_offset),
                static_cast<std::size_t>(d.stored_size));
            const auto dst = std::span{out}.subspan(at, static_cast<std::size_t>(d.raw_size));
            if (d.codec_id == codec::store_id) {
                std::ranges::copy(in, dst.begin());
            } else if (auto r = decoder.decode(d.codec_id, in, dst); !r) {
                return std::unexpected(r.error());
            }
            if (seedarc::experimental::crc32c(dst) != d.raw_checksum) {
                return std::unexpected(bad("crc mismatch"));
            }
            at += dst.size();
        }
        return out;
    }
    return std::unexpected(Error{ErrorCode::invalid_argument, "no such file in archive"});
}

int peek(const fs::path &container_path, const fs::path &input, std::size_t n, bool sarc) {
    auto originals = read_tree(input);
    if (!originals || originals->empty()) {
        return fail(originals ? bad("empty input") : originals.error());
    }
    std::vector<double> times;
    const auto step = std::max<std::size_t>(1, originals->size() / std::max<std::size_t>(1, n));
    for (std::size_t i = 0; i < originals->size() && times.size() < n; i += step) {
        const auto &want = (*originals)[i];
        const auto w = wall_ms();
        std::vector<std::byte> got;
        if (sarc) {
            auto r = sarc_one(container_path, want.path);
            if (!r) {
                return fail(r.error());
            }
            got = std::move(*r);
        } else {
            // Cold per call: read the container, parse the index, load the dictionary.
            auto bytes = read_file(container_path);
            if (!bytes) {
                return fail(bytes.error());
            }
            auto c = parse(*bytes);
            if (!c) {
                return fail(c.error());
            }
            auto d = make_decoders(*c);
            if (!d) {
                return fail(d.error());
            }
            const auto it = std::ranges::find(c->entries, want.path, &Entry::path);
            if (it == c->entries.end()) {
                return fail(bad("file missing"));
            }
            if (auto r = decode_entry(*c, *d, *it, *bytes, got); !r) {
                return fail(r.error());
            }
        }
        times.push_back(wall_ms() - w);
        if (got != want.data) {
            return fail(bad("single-file extraction mismatch"));
        }
    }
    std::ranges::sort(times);
    const auto q = [&](double p) {
        return times[std::min(times.size() - 1,
                              static_cast<std::size_t>(p * (times.size() - 1) + 0.5))];
    };
    std::cout << "{\"schema\":\"seedarc.dict-study.r1\",\"mode\":\""
              << (sarc ? "peek-sarc" : "peek")
              << "\",\"byte_exact\":true,\"samples\":" << times.size()
              << ",\"median_ms\":" << q(0.5) << ",\"p90_ms\":" << q(0.9)
              << ",\"max_ms\":" << times.back() << "," << peak_memory_json() << "}\n";
    return std::cout ? 0 : 1;
}

std::optional<std::uint64_t> number(std::string_view text) {
    std::uint64_t v = 0;
    if (text.empty() || text.size() > 12) {
        return std::nullopt;
    }
    for (const auto c : text) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        v = v * 10 + static_cast<std::uint64_t>(c - '0');
    }
    return v;
}
} // namespace

int main(int argc, char *argv[]) {
    const std::vector<std::string_view> a(argv, argv + argc);
    if (a.size() == 7 && a[1] == "pack") {
        const auto level = number(a[4]), dict = number(a[5]), train = number(a[6]);
        if (!level || *level < 1 || *level > 19 || !dict ||
            (*dict != 0 && *dict < research::min_dictionary_bytes) ||
            *dict > research::max_dictionary_bytes || !train) {
            std::cerr << usage;
            return 2;
        }
        return pack(native(a[2]), native(a[3]), static_cast<std::uint32_t>(*level),
                    static_cast<std::size_t>(*dict), *train);
    }
    if (a.size() == 4 && a[1] == "verify") {
        return verify(native(a[2]), native(a[3]));
    }
    if (a.size() == 5 && (a[1] == "peek" || a[1] == "peek-sarc")) {
        const auto n = number(a[4]);
        if (!n || *n == 0 || *n > 10000) {
            std::cerr << usage;
            return 2;
        }
        return peek(native(a[2]), native(a[3]), static_cast<std::size_t>(*n), a[1] == "peek-sarc");
    }
    std::cerr << usage;
    return 2;
}
