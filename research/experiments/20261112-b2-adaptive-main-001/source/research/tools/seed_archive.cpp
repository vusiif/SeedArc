// SPDX-License-Identifier: GPL-3.0-or-later
#include "archive_plan.hpp"
#include "seed/adaptive_file.hpp"
#include "seed/archive_study.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string_view>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <psapi.h>
#include <shellapi.h>

namespace fs = std::filesystem;
namespace study = seedarc::experimental::seed_study;
using namespace seedarc::experimental;
namespace {
constexpr std::string_view usage =
    "Experimental concrete-plan compression; no seed wire format or search.\n"
    "Usage: seedarc_seed_archive compress MODE INPUT ARCHIVE TARGET_PATH|PLAN_FILE|-\n"
    "       seedarc_seed_archive verify ARCHIVE\n"
    "       seedarc_seed_archive extract ARCHIVE NEW_DIRECTORY\n"
    "       seedarc_seed_archive --version | --help\n"
    "MODE: store, zstd3, zstd6, lzma6, b-base, b-global, b-file, b-cleared, file-plan,\n"
    "      adaptive-file (TARGET = trial budget bytes or 'unbounded'), oracle-file (TARGET -).\n"
    "64 KiB chunks. B-r1 codec seed0, LZMA2 preset6. Zstd levels: base3, global4,\n"
    "file6 on TARGET_PATH over global4, cleared4. Target required only for b-file.\n"
    "file-plan reads PLAN_FILE: one UTF-8 line per regular file,\n"
    "'SOURCE CODEC CONFIG PATH' (SOURCE file_codec|file_override, CODEC 0|1|2);\n"
    "every file must appear exactly once. Paths are exact canonical relative archive paths.\n";

constexpr std::size_t max_plan_file_bytes = 4U * 1024 * 1024;

struct FilePlan {
    std::string source;
    internal::BlockEncoding encoding;
};

// Research-only concrete per-file plan; parsing is bounded and strict, never a wire format.
std::expected<std::map<std::string, FilePlan, std::less<>>, seedarc::Error>
read_file_plan(const fs::path &path) {
    const auto fail = [](const char *message) {
        return std::unexpected(seedarc::Error{seedarc::ErrorCode::invalid_argument, message});
    };
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return std::unexpected(seedarc::Error{seedarc::ErrorCode::io_error, "cannot open plan"});
    }
    std::string text(max_plan_file_bytes + 1, '\0');
    stream.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (stream.bad()) {
        return std::unexpected(seedarc::Error{seedarc::ErrorCode::io_error, "cannot read plan"});
    }
    text.resize(static_cast<std::size_t>(stream.gcount()));
    if (text.size() > max_plan_file_bytes) {
        return std::unexpected(
            seedarc::Error{seedarc::ErrorCode::resource_limit, "plan too large"});
    }
    std::map<std::string, FilePlan, std::less<>> plans;
    std::string_view rest{text};
    while (!rest.empty()) {
        const auto end = rest.find('\n');
        if (end == std::string_view::npos) {
            return fail("plan line lacks a newline");
        }
        const auto line = rest.substr(0, end);
        rest.remove_prefix(end + 1);
        std::array<std::string_view, 3> fields{};
        auto tail = line;
        for (auto &field : fields) {
            const auto space = tail.find(' ');
            if (space == std::string_view::npos || space == 0) {
                return fail("malformed plan line");
            }
            field = tail.substr(0, space);
            tail.remove_prefix(space + 1);
        }
        if (fields[0] != "file_codec" && fields[0] != "file_override") {
            return fail("unknown plan source");
        }
        const auto number = [](std::string_view digits) -> std::optional<std::uint32_t> {
            if (digits.empty() || digits.size() > 2 ||
                !std::ranges::all_of(digits, [](char c) { return c >= '0' && c <= '9'; })) {
                return std::nullopt;
            }
            std::uint32_t value = 0;
            for (const auto c : digits) {
                value = value * 10 + static_cast<std::uint32_t>(c - '0');
            }
            return value;
        };
        const auto codec = number(fields[1]);
        const auto config = number(fields[2]);
        if (!codec || *codec > 2 || !config || tail.empty()) {
            return fail("invalid plan codec, config or path");
        }
        if (plans.size() == study::max_scope_files) {
            return std::unexpected(
                seedarc::Error{seedarc::ErrorCode::resource_limit, "plan file count limit"});
        }
        // Codec/config legality is checked again by the core before any content is read.
        if (!plans
                 .emplace(std::string(tail),
                          FilePlan{std::string(fields[0]), {static_cast<Codec>(*codec), *config}})
                 .second) {
            return fail("duplicate plan path");
        }
    }
    return plans;
}

void json_string(std::string_view value) {
    std::cout << '"';
    for (const auto raw : value) {
        const auto c = static_cast<unsigned char>(raw);
        if (c == '"' || c == '\\') {
            std::cout << '\\' << raw;
        } else if (c < 32) {
            constexpr char hex[] = "0123456789abcdef";
            std::cout << "\\u00" << hex[c >> 4] << hex[c & 15];
        } else {
            std::cout << raw;
        }
    }
    std::cout << '"';
}
double wall_ms() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
double filetime_ms(const FILETIME &a, const FILETIME &b) {
    const auto ticks = [](const FILETIME &t) {
        return (static_cast<std::uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
    };
    return static_cast<double>(ticks(a) + ticks(b)) / 10000.0; // 100 ns units
}
// User + kernel CPU of this process / thread in ms; negative if unavailable.
double process_cpu_ms() {
    FILETIME created{}, exited{}, kernel{}, user{};
    return GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)
               ? filetime_ms(kernel, user)
               : -1.0;
}
double thread_cpu_ms() {
    FILETIME created{}, exited{}, kernel{}, user{};
    return GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)
               ? filetime_ms(kernel, user)
               : -1.0;
}
void json_number(std::optional<double> value) {
    if (value && *value >= 0) {
        std::cout << *value;
    } else {
        std::cout << "null";
    }
}
void timing(double wall_start, double cpu_start) {
    std::cout << ",\"call_wall_ms\":" << wall_ms() - wall_start << ",\"call_cpu_ms\":";
    json_number(cpu_start < 0 ? std::nullopt : std::optional{process_cpu_ms() - cpu_start});
}
std::string_view codec_name(Codec codec) {
    return codec == Codec::store ? "store" : codec == Codec::zstd ? "zstd" : "lzma2";
}
void selector_json(const study::SelectorReport &r) {
    std::cout << ",\"selector\":{\"selector_version\":";
    json_string(r.selector_version);
    std::cout << ",\"budget_limit\":";
    if (r.budget_limit) {
        std::cout << *r.budget_limit;
    } else {
        std::cout << "null";
    }
    std::cout << ",\"budget_used\":" << r.budget_used << ",\"trial_count\":" << r.trial_count
              << ",\"read_bytes\":" << r.read_bytes
              << ",\"budget_exhausted_files\":" << r.budget_exhausted_files
              << ",\"decision_wall_ms\":" << r.wall_ms << ",\"decision_cpu_ms\":";
    json_number(r.cpu_ms);
    std::cout << ",\"files\":[";
    for (std::size_t i = 0; i < r.files.size(); ++i) {
        const auto &f = r.files[i];
        std::cout << (i != 0 ? "," : "") << "{\"file_id\":" << f.file_id << ",\"file_key\":";
        json_string(f.file_key);
        std::cout << ",\"file_bytes\":" << f.file_bytes << ",\"selected_codec\":";
        json_string(codec_name(f.codec));
        std::cout << ",\"selected_level\":" << f.level << ",\"decision_reason\":";
        json_string(f.reason);
        std::cout << ",\"notes\":[";
        for (std::size_t n = 0; n < f.notes.size(); ++n) {
            std::cout << (n != 0 ? "," : "");
            json_string(f.notes[n]);
        }
        std::cout << "],\"sampled_blocks\":[";
        for (std::size_t n = 0; n < f.sampled_blocks.size(); ++n) {
            std::cout << (n != 0 ? "," : "") << f.sampled_blocks[n];
        }
        std::cout << "],\"read_bytes\":" << f.read_bytes << ",\"trials\":[";
        for (std::size_t n = 0; n < f.trials.size(); ++n) {
            const auto &t = f.trials[n];
            std::cout << (n != 0 ? "," : "") << "{\"file_block_index\":" << t.file_block_index
                      << ",\"trial_codec\":";
            json_string(codec_name(t.codec));
            std::cout << ",\"trial_level\":" << t.level
                      << ",\"trial_input_bytes\":" << t.input_bytes
                      << ",\"trial_output_bytes\":" << t.output_bytes
                      << ",\"encoded\":" << (t.encoded ? "true" : "false") << '}';
        }
        std::cout << "],\"budget_used_after\":" << f.budget_used_after
                  << ",\"trial_wall_ms\":" << f.wall_ms << ",\"trial_cpu_ms\":";
        json_number(f.cpu_ms);
        std::cout << '}';
    }
    std::cout << "]}";
}
void peak_memory() {
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    std::cout << ",\"peak_working_set_bytes\":";
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
        std::cout << counters.PeakWorkingSetSize;
    } else {
        std::cout << "null";
    }
}
int error(const seedarc::Error &e) {
    std::cerr << e.message << '\n';
    return 1;
}
struct ArchiveLayout {
    ArchiveHeader header;
    ArchiveIndex index;
};

std::expected<ArchiveLayout, seedarc::Error> read_index(const fs::path &archive) {
    std::ifstream stream(archive, std::ios::binary);
    std::array<std::byte, archive_header_bytes> bytes{};
    if (!stream.read(reinterpret_cast<char *>(bytes.data()), bytes.size())) {
        return std::unexpected(seedarc::Error{seedarc::ErrorCode::io_error, "read header failed"});
    }
    const auto header = decode_header(bytes);
    if (!header) {
        return std::unexpected(header.error());
    }
    stream.seekg(static_cast<std::streamoff>(archive_header_bytes + header->data_bytes));
    std::vector<std::byte> index(static_cast<std::size_t>(header->index_bytes));
    if (!stream.read(reinterpret_cast<char *>(index.data()),
                     static_cast<std::streamsize>(index.size()))) {
        return std::unexpected(seedarc::Error{seedarc::ErrorCode::io_error, "read index failed"});
    }
    auto parsed = parse_index(*header, index);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    return ArchiveLayout{*header, std::move(*parsed)};
}
fs::path native_path(std::string_view text) {
    return fs::path{std::u8string{text.begin(), text.end()}};
}

int run(int argc, char *argv[]) {
    if (argc == 2 && std::string_view{argv[1]} == "--help") {
        std::cout << usage;
        return 0;
    }
    if (argc == 2 && std::string_view{argv[1]} == "--version") {
        std::cout << "SeedArc B2-r5 study; " << codec_library_versions() << '\n';
        return 0;
    }
    if ((argc == 3 && std::string_view{argv[1]} == "verify") ||
        (argc == 4 && std::string_view{argv[1]} == "extract")) {
        const auto wall_start = wall_ms();
        const auto cpu_start = process_cpu_ms();
        const auto result = argc == 3 ? verify_archive(native_path(argv[2]))
                                      : extract_archive(native_path(argv[2]), native_path(argv[3]));
        if (!result) {
            return error(result.error());
        }
        std::cout << "{\"archive_bytes\":" << result->archive_bytes;
        timing(wall_start, cpu_start);
        peak_memory();
        std::cout << "}\n";
        return std::cout ? 0 : 1;
    }
    if (argc != 6 || std::string_view{argv[1]} != "compress") {
        std::cerr << usage;
        return 2;
    }
    const std::string_view mode{argv[2]}, target{argv[5]};
    const bool b_mode =
        mode == "b-base" || mode == "b-global" || mode == "b-file" || mode == "b-cleared";
    const bool fixed = mode == "store" || mode == "zstd3" || mode == "zstd6" || mode == "lzma6";
    const bool file_plan = mode == "file-plan";
    const bool adaptive = mode == "adaptive-file";
    const bool oracle = mode == "oracle-file";
    if ((!b_mode && !fixed && !file_plan && !adaptive && !oracle) ||
        (mode == "b-file" || file_plan || adaptive ? target == "-" : target != "-")) {
        std::cerr << usage;
        return 2;
    }
    CompressOptions options;
    options.chunk_size_bytes = 65536;
    study::B2Compression report;
    // Research label of the rule that produced each block's planned encoding (by file ID).
    std::map<std::uint64_t, std::string> file_sources;
    const auto input = native_path(argv[3]), archive = native_path(argv[4]);
    std::optional<study::SelectorReport> selection;
    const auto wall_start = wall_ms();
    const auto cpu_start = process_cpu_ms();
    if (adaptive || oracle) {
        study::SelectorConfig config;
        config.kind = oracle ? study::SelectorKind::oracle : study::SelectorKind::adaptive;
        if (adaptive && target == "unbounded") {
            config.trial_budget_bytes = study::unbounded_trial_budget;
        } else if (adaptive) {
            const auto *end = target.data() + target.size();
            const auto parsed = std::from_chars(target.data(), end, config.trial_budget_bytes);
            if (parsed.ec != std::errc{} || parsed.ptr != end ||
                config.trial_budget_bytes == study::unbounded_trial_budget) {
                std::cerr << usage;
                return 2;
            }
        }
        if (thread_cpu_ms() >= 0) {
            config.cpu_ms = thread_cpu_ms;
        }
        auto result = study::compress_selected_archive(input, archive, options, config);
        if (!result) {
            return error(result.error());
        }
        report.summary = result->summary;
        report.files = std::move(result->files);
        report.planned = std::move(result->planned);
        selection = std::move(result->report);
    } else if (file_plan) {
        auto plans = read_file_plan(native_path(target));
        if (!plans) {
            return error(plans.error());
        }
        const internal::PlanBuilder builder = [&](std::span<const IndexEntry> entries,
                                                  std::uint64_t count)
            -> std::expected<std::vector<internal::BlockEncoding>, seedarc::Error> {
            if (count > study::max_scope_blocks || entries.size() > study::max_scope_files) {
                return std::unexpected(
                    seedarc::Error{seedarc::ErrorCode::resource_limit, "study limit"});
            }
            std::vector<internal::BlockEncoding> result;
            result.reserve(static_cast<std::size_t>(count));
            std::size_t used = 0;
            for (std::size_t i = 0; i < entries.size(); ++i) {
                if (entries[i].directory) {
                    continue;
                }
                const auto found = plans->find(entries[i].path);
                if (found == plans->end()) {
                    return std::unexpected(seedarc::Error{seedarc::ErrorCode::invalid_argument,
                                                          "plan misses file: " + entries[i].path});
                }
                ++used;
                report.files.push_back({i, entries[i].path});
                file_sources[i] = found->second.source;
                const auto &encoding = found->second.encoding;
                for (std::uint64_t local = 0; local < entries[i].block_count; ++local) {
                    report.planned.push_back({{i, local, entries[i].first_block + local},
                                              {encoding.codec, encoding.config}});
                    result.push_back(encoding);
                }
            }
            if (used != plans->size()) {
                return std::unexpected(seedarc::Error{seedarc::ErrorCode::invalid_argument,
                                                      "plan names a path not in the input"});
            }
            return result;
        };
        const auto result = internal::compress_archive_planned(input, archive, options, builder);
        if (!result) {
            return error(result.error());
        }
        report.summary = *result;
    } else if (b_mode) {
        std::vector<study::PathRankOverride> edits;
        if (mode == "b-file") {
            edits.push_back({std::string(target), 5, std::nullopt});
        }
        auto result = study::compress_b2_archive(input, archive, options,
                                                 {0, mode == "b-base" ? 2U : 3U, 6}, edits);
        if (!result) {
            return error(result.error());
        }
        report = std::move(*result);
    } else {
        const auto codec = mode == "store"   ? Codec::store
                           : mode == "lzma6" ? Codec::lzma2
                                             : Codec::zstd;
        const std::uint32_t config = mode == "store" ? 0U : mode == "zstd3" ? 3U : 6U;
        const internal::PlanBuilder builder = [&](std::span<const IndexEntry> entries,
                                                  std::uint64_t count)
            -> std::expected<std::vector<internal::BlockEncoding>, seedarc::Error> {
            if (count > study::max_scope_blocks || entries.size() > study::max_scope_files) {
                return std::unexpected(
                    seedarc::Error{seedarc::ErrorCode::resource_limit, "study limit"});
            }
            for (std::size_t i = 0; i < entries.size(); ++i) {
                if (!entries[i].directory) {
                    report.files.push_back({i, entries[i].path});
                    for (std::uint64_t local = 0; local < entries[i].block_count; ++local) {
                        report.planned.push_back(
                            {{i, local, entries[i].first_block + local}, {codec, config}});
                    }
                }
            }
            return std::vector<internal::BlockEncoding>(static_cast<std::size_t>(count),
                                                        {codec, config});
        };
        const auto result = internal::compress_archive_planned(input, archive, options, builder);
        if (!result) {
            return error(result.error());
        }
        report.summary = *result;
    }
    const auto call_wall_ms = wall_ms() - wall_start;
    const auto call_cpu_ms = cpu_start < 0 ? -1.0 : process_cpu_ms() - cpu_start;
    const auto layout = read_index(archive);
    if (!layout) {
        return error(layout.error());
    }
    const auto *actual = &layout->index;
    if (actual->blocks.size() != report.planned.size()) {
        std::cerr << "plan and archive count mismatch\n";
        return 1;
    }
    std::map<std::uint64_t, std::string_view> paths;
    for (const auto &file : report.files) {
        paths[file.file_id] = file.path;
    }
    const auto plan_source = [&](const study::PlannedBlock &p) -> std::string_view {
        if (fixed) {
            return "fixed";
        }
        if (file_plan) {
            return file_sources.at(p.identity.file_id);
        }
        if (adaptive || oracle) {
            return adaptive ? "adaptive_file" : "oracle_file";
        }
        if (mode == "b-base") {
            return "base";
        }
        // b-file overrides only the Zstd rank, so only Zstd blocks of the target take it.
        if (mode == "b-file" && p.plan.codec == Codec::zstd &&
            paths.at(p.identity.file_id) == target) {
            return "file_override";
        }
        return "global";
    };
    const auto &header = layout->header;
    std::cout << "{\"schema\":\"seedarc.b2-compression.r5\",\"mode\":";
    json_string(mode);
    std::cout << ",\"chunk_bytes\":65536,\"archive_bytes\":" << report.summary.archive_bytes
              << ",\"input_bytes\":" << report.summary.total_output
              << ",\"header_bytes\":" << archive_header_bytes
              << ",\"data_bytes\":" << header.data_bytes
              << ",\"index_bytes\":" << header.index_bytes
              << ",\"entry_count\":" << header.entry_count
              << ",\"block_count\":" << header.block_count
              << ",\"strategy_version\":" << header.strategy_version
              << ",\"seed_length\":" << header.seed_length << ",\"files\":[";
    for (std::size_t i = 0; i < report.files.size(); ++i) {
        if (i != 0) {
            std::cout << ',';
        }
        std::cout << "{\"file_id\":" << report.files[i].file_id << ",\"path\":";
        json_string(report.files[i].path);
        std::cout << '}';
    }
    std::cout << "],\"blocks\":[";
    for (std::size_t i = 0; i < report.planned.size(); ++i) {
        if (i != 0) {
            std::cout << ',';
        }
        const auto &p = report.planned[i];
        const auto &a = actual->blocks[i];
        std::cout << "{\"block_index\":" << i << ",\"block_id\":" << p.identity.global_block_id
                  << ",\"file_id\":" << p.identity.file_id
                  << ",\"file_block_index\":" << p.identity.file_block_index << ",\"file_key\":";
        json_string(paths.at(p.identity.file_id));
        std::cout << ",\"plan_source\":";
        json_string(plan_source(p));
        std::cout << ",\"planned_codec\":" << static_cast<std::uint32_t>(p.plan.codec)
                  << ",\"planned_config\":" << p.plan.level << ",\"actual_codec\":" << a.codec_id
                  << ",\"actual_config\":" << a.codec_config_id << ",\"raw_bytes\":" << a.raw_size
                  << ",\"stored_bytes\":" << a.stored_size << '}';
    }
    std::cout << "],\"call_wall_ms\":" << call_wall_ms << ",\"call_cpu_ms\":";
    json_number(call_cpu_ms);
    if (selection) {
        selector_json(*selection);
    } else {
        std::cout << ",\"selector\":null";
    }
    peak_memory();
    std::cout << "}\n";
    return std::cout ? 0 : 1;
}

} // namespace

int main() {
    int count = 0;
    const auto free_args = [](wchar_t **p) { LocalFree(p); };
    std::unique_ptr<wchar_t *, decltype(free_args)> wide(
        CommandLineToArgvW(GetCommandLineW(), &count), free_args);
    if (!wide) {
        return 1;
    }
    std::vector<std::string> utf8;
    utf8.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.get()[i], -1,
                                             nullptr, 0, nullptr, nullptr);
        if (size == 0) {
            return 1;
        }
        std::string text(static_cast<std::size_t>(size), '\0');
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.get()[i], -1, text.data(), size,
                                nullptr, nullptr) != size) {
            return 1;
        }
        text.pop_back();
        utf8.push_back(std::move(text));
    }
    std::vector<char *> args;
    args.reserve(utf8.size());
    for (auto &text : utf8) {
        args.push_back(text.data());
    }
    return run(count, args.data());
}
