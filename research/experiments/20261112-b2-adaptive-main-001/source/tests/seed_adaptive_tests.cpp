// SPDX-License-Identifier: GPL-3.0-or-later
#include "seed/adaptive_file.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace fs = std::filesystem;
namespace study = seedarc::experimental::seed_study;
using namespace seedarc::experimental;
using seedarc::ErrorCode;
namespace {
int failures = 0;
void check(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}
std::vector<std::byte> read(const fs::path &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("fixture read failed");
    }
    std::vector<char> chars{std::istreambuf_iterator<char>{file}, {}};
    std::vector<std::byte> bytes(chars.size());
    std::ranges::transform(chars, bytes.begin(), [](char c) { return static_cast<std::byte>(c); });
    return bytes;
}
void write(const fs::path &path, std::span<const std::byte> bytes) {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    if (!file) {
        throw std::runtime_error("fixture write failed");
    }
}
std::uint64_t next(std::uint64_t &state) {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL; // fixed 64-bit LCG
    return state >> 33;
}
// Words from a small vocabulary: compressible, with structure LZMA2 typically models better.
constexpr std::array<std::string_view, 16> words{
    "seed ", "archive ", "block ", "codec ", "level ",  "plan ",   "store ",  "zstd ",
    "lzma ", "file ",    "index ", "trial ", "budget ", "sample ", "header ", "data\n"};
std::vector<std::byte> vocabulary_text(std::size_t size) {
    std::uint64_t state = 42;
    std::vector<std::byte> out;
    while (out.size() < size) {
        for (const auto c : words[next(state) % words.size()]) {
            out.push_back(static_cast<std::byte>(c));
        }
    }
    out.resize(size);
    return out;
}
std::vector<std::byte> noise(std::size_t size) {
    std::uint64_t state = 7;
    std::vector<std::byte> out(size);
    for (auto &b : out) {
        b = static_cast<std::byte>(next(state) & 0xff);
    }
    return out;
}

// Re-derives adaptive-file-r1's decision from the recorded trials (independent of the code).
std::string expected_reason(const study::FileDecision &d, const study::SelectorConfig &c) {
    std::map<std::pair<Codec, std::uint32_t>, std::uint64_t> sums;
    std::uint64_t sample = 0;
    for (const auto &t : d.trials) {
        sums[{t.codec, t.level}] += t.output_bytes;
        if (t.codec == Codec::zstd && t.level == 3) {
            sample += t.input_bytes;
        }
    }
    const auto z3 = sums[{Codec::zstd, 3}];
    if (z3 == sample && sums[{Codec::lzma2, 6}] == sample) {
        return "no_gain";
    }
    const auto ok = [&](std::uint64_t v) {
        return v < z3 && (z3 - v) * 1000 >= z3 * c.min_gain_permille &&
               (z3 - v) * d.file_bytes >= c.min_gain_bytes * sample;
    };
    const auto l6 = sums[{Codec::lzma2, 6}];
    const bool has_z6 = sums.contains({Codec::zstd, 6});
    const auto z6 = has_z6 ? sums[{Codec::zstd, 6}] : 0;
    const bool take_l6 = ok(l6), take_z6 = has_z6 && ok(z6);
    if (take_l6 && (!take_z6 || l6 <= z6)) {
        return "lzma2_6_saves";
    }
    return take_z6 ? "zstd_6_saves" : "insufficient_gain";
}
} // namespace

int main(int argc, char *argv[]) {
    if (argc != 2) {
        return 2;
    }
    const auto parent = fs::absolute(argv[1]);
    fs::create_directories(parent);
    const auto work = parent / ("seed-adaptive-" + std::to_string(GetCurrentProcessId()));
    if (!fs::create_directory(work)) {
        std::cerr << "test directory already exists\n";
        return 1;
    }
    try {
        const auto input = work / "input";
        fs::create_directory(input);
        const auto vocab = vocabulary_text(80 * 1024);
        const auto random = noise(36 * 1024);
        const std::vector<std::byte> short_text(1000, std::byte{'s'});
        const std::vector<std::byte> repeat(40 * 1024, std::byte{'a'});
        write(input / "a-vocab.txt", vocab);
        write(input / "b-noise.bin", random);
        write(input / "c-small.txt", short_text);
        write(input / "d-empty", {});
        write(input / "e-repeat.txt", repeat);
        CompressOptions options;
        options.chunk_size_bytes = 4096;
        study::SelectorConfig unbounded;
        unbounded.trial_budget_bytes = study::unbounded_trial_budget;

        const auto first =
            study::compress_selected_archive(input, work / "u1.sarc", options, unbounded);
        const auto second =
            study::compress_selected_archive(input, work / "u2.sarc", options, unbounded);
        if (!first || !second) {
            throw std::runtime_error("unbounded selection failed");
        }
        check(read(work / "u1.sarc") == read(work / "u2.sarc") && first->planned == second->planned,
              "same input and parameters give the same plan and archive");
        const auto &r = first->report;
        check(r.selector_version == study::adaptive_selector_version && !r.budget_limit,
              "unbounded adaptive report");
        std::map<std::string, const study::FileDecision *> by;
        std::uint64_t trial_bytes = 0;
        for (const auto &f : r.files) {
            by[f.file_key] = &f;
            for (const auto &t : f.trials) {
                trial_bytes += t.input_bytes;
                check(t.encoded == (t.output_bytes < t.input_bytes) &&
                          t.output_bytes <= t.input_bytes,
                      "trial length follows the writer's strictly-smaller rule");
            }
        }
        check(by.size() == 5 && trial_bytes == r.budget_used, "every trial byte is accounted for");
        check(by["d-empty"]->reason == "empty_file" && by["d-empty"]->trials.empty(),
              "empty file falls back without trials");
        check(by["c-small.txt"]->reason == "below_min_size" && by["c-small.txt"]->trials.empty(),
              "small file keeps Zstd-3 without trials");
        check(by["b-noise.bin"]->reason == "no_gain" && by["b-noise.bin"]->codec == Codec::zstd &&
                  by["b-noise.bin"]->level == 3,
              "incompressible file keeps Zstd-3 (writer falls back to Store)");
        // 80 KiB / 4 KiB = 20 blocks: first, middle, last.
        check(by["a-vocab.txt"]->sampled_blocks == std::vector<std::uint64_t>{0, 10, 19},
              "fixed head / middle / tail sample");
        check(by["a-vocab.txt"]->reason == "lzma2_6_saves" &&
                  by["a-vocab.txt"]->codec == Codec::lzma2,
              "LZMA2-6 switch path is exercised");
        for (const auto &f : r.files) {
            if (!f.trials.empty()) {
                check(f.reason == expected_reason(f, unbounded),
                      "decision follows the registered rule");
            }
            check(f.read_bytes <= f.file_bytes, "selection reads at most the file once");
        }
        for (const auto &p : first->planned) {
            const auto &f = r.files[static_cast<std::size_t>(
                std::ranges::find(r.files, p.identity.file_id, &study::FileDecision::file_id) -
                r.files.begin())];
            check(p.plan.codec == f.codec && p.plan.level == f.level,
                  "whole file takes the file decision");
        }
        const auto out = work / "u1-out";
        check(verify_archive(work / "u1.sarc").has_value() &&
                  extract_archive(work / "u1.sarc", out).has_value(),
              "decoded without the selector");
        check(read(out / "a-vocab.txt") == vocab && read(out / "b-noise.bin") == random &&
                  read(out / "c-small.txt") == short_text && read(out / "d-empty").empty() &&
                  read(out / "e-repeat.txt") == repeat,
              "round trip is byte exact");

        // Zero budget: deterministic Zstd-3 fallback, byte-identical to the fixed public path.
        study::SelectorConfig zero;
        const auto none = study::compress_selected_archive(input, work / "z.sarc", options, zero);
        auto fixed = options;
        fixed.codec = Codec::zstd;
        fixed.level = 3;
        const auto plain = compress_archive(input, work / "fixed.sarc", fixed);
        check(none && plain && read(work / "z.sarc") == read(work / "fixed.sarc"),
              "zero budget equals fixed Zstd-3 byte for byte");
        check(none && none->report.budget_used == 0 && none->report.trial_count == 0 &&
                  none->report.budget_exhausted_files == 3,
              "zero budget records three exhausted eligible files");

        // A budget that fits exactly one eligible file: the first in manifest order.
        constexpr auto vocab_cost = 2 * 3 * 4096ULL;
        study::SelectorConfig one;
        one.trial_budget_bytes = vocab_cost;
        const auto partial = study::compress_selected_archive(input, work / "p.sarc", options, one);
        if (!partial) {
            throw std::runtime_error("partial-budget selection failed");
        }
        check(partial->report.budget_used <= one.trial_budget_bytes &&
                  partial->report.budget_exhausted_files == 2,
              "budget is never exceeded and later files fall back");
        check(partial->report.files[0].notes == std::vector<std::string>{"zstd6_skipped_budget"},
              "Zstd-6 trial is skipped when the remaining budget is short");

        // Oracle: every block of every non-empty file, three configurations, unbounded.
        study::SelectorConfig oracle_config;
        oracle_config.kind = study::SelectorKind::oracle;
        const auto oracle =
            study::compress_selected_archive(input, work / "o.sarc", options, oracle_config);
        if (!oracle) {
            throw std::runtime_error("oracle selection failed");
        }
        std::uint64_t total = 0;
        for (const auto &f : oracle->report.files) {
            total += f.file_bytes;
        }
        check(oracle->report.selector_version == study::oracle_selector_version &&
                  !oracle->report.budget_limit && oracle->report.budget_used == 3 * total,
              "oracle trials every byte three times");
        check(verify_archive(work / "o.sarc").has_value(), "oracle archive verifies");

        // Invalid parameters and cancellation publish nothing.
        auto bad = unbounded;
        bad.min_gain_permille = 1001;
        const auto invalid =
            study::compress_selected_archive(input, work / "bad.sarc", options, bad);
        check(!invalid && invalid.error().code == ErrorCode::invalid_argument &&
                  !fs::exists(work / "bad.sarc"),
              "invalid selector parameters rejected");
        std::stop_source source;
        source.request_stop();
        const auto stopped = study::compress_selected_archive(input, work / "stop.sarc", options,
                                                              unbounded, source.get_token());
        check(!stopped && stopped.error().code == ErrorCode::cancelled &&
                  !fs::exists(work / "stop.sarc"),
              "cancellation publishes nothing");
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        ++failures;
    }
    bool safe = work.parent_path() == parent;
    for (const auto &entry : fs::recursive_directory_iterator(work)) {
        const auto attrs = GetFileAttributesW(entry.path().c_str());
        safe =
            safe && attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
    }
    if (safe && failures == 0) {
        fs::remove_all(work);
    } else {
        std::cerr << "fixture retained at " << work << '\n';
    }
    std::cout << (failures == 0 ? "all seed adaptive checks passed" : "seed adaptive checks failed")
              << '\n';
    return failures == 0 ? 0 : 1;
}
