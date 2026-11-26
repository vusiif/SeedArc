// SPDX-License-Identifier: GPL-3.0-or-later
// ADR-0005: conformance of the archive path profile's Unicode operations with the pinned UCD.
// Usage: seedarc_unicode_tests <path to NormalizationTest.txt>
#include "filesystem/unicode_profile.hpp"

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <iostream>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace unicode = seedarc::experimental::internal::unicode;

namespace {
int failures = 0;

void check(bool condition, const std::string &description) {
    if (!condition) {
        if (++failures <= 20) {
            std::cerr << "FAIL: " << description << '\n';
        }
    }
}

std::string hex(std::u32string_view text) {
    std::string out;
    for (const auto c : text) {
        char buffer[16];
        const auto [end, ec] =
            std::to_chars(buffer, buffer + sizeof(buffer), static_cast<std::uint32_t>(c), 16);
        out.append(buffer, end);
        out += ' ';
    }
    return out;
}

std::optional<std::u32string> parse_field(std::string_view field) {
    std::u32string out;
    while (!field.empty()) {
        while (!field.empty() && field.front() == ' ') {
            field.remove_prefix(1);
        }
        if (field.empty()) {
            break;
        }
        std::uint32_t value = 0;
        const auto [end, ec] =
            std::from_chars(field.data(), field.data() + field.size(), value, 16);
        if (ec != std::errc{}) {
            return std::nullopt;
        }
        out.push_back(static_cast<char32_t>(value));
        field.remove_prefix(static_cast<std::size_t>(end - field.data()));
    }
    return out;
}
} // namespace

int main(int argc, char *argv[]) {
    if (argc != 2) {
        std::cerr << "usage: seedarc_unicode_tests <NormalizationTest.txt>\n";
        return 2;
    }
    check(unicode::ucd_version() == "17.0.0", "pinned UCD version");
    std::ifstream file{argv[1]};
    check(file.good(), "NormalizationTest.txt opened");
    std::string line;
    bool in_part1 = false;
    std::set<char32_t> part1;
    std::size_t cases = 0;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.starts_with("@Part")) {
            in_part1 = line.starts_with("@Part1");
            continue;
        }
        if (line.empty() || line.front() == '#') {
            continue;
        }
        std::vector<std::u32string> columns;
        std::string_view rest{line};
        for (int i = 0; i < 5; ++i) {
            const auto semicolon = rest.find(';');
            const auto field = parse_field(rest.substr(0, semicolon));
            check(field.has_value() && semicolon != std::string_view::npos, "parse: " + line);
            if (!field || semicolon == std::string_view::npos) {
                break;
            }
            columns.push_back(*field);
            rest.remove_prefix(semicolon + 1);
        }
        if (columns.size() != 5) {
            continue;
        }
        ++cases;
        // UAX #15 conformance for NFC: c2 == NFC(c1) == NFC(c2) == NFC(c3); c4 == NFC(c4) ==
        // NFC(c5).
        const auto &[c1, c2, c3, c4, c5] =
            std::tie(columns[0], columns[1], columns[2], columns[3], columns[4]);
        for (const auto *source : {&c1, &c2, &c3}) {
            check(unicode::to_nfc(*source) == c2, "NFC(" + hex(*source) + ") == " + hex(c2));
        }
        for (const auto *source : {&c4, &c5}) {
            check(unicode::to_nfc(*source) == c4, "NFC(" + hex(*source) + ") == " + hex(c4));
        }
        check(unicode::is_nfc(c2) && unicode::is_nfc(c4), "c2 / c4 are NFC: " + line);
        check(unicode::is_nfc(c1) == (c1 == c2), "is_nfc(c1) agrees with c1 == c2: " + line);
        if (in_part1 && c1.size() == 1) {
            part1.insert(c1[0]);
        }
    }
    check(cases > 19000, "NormalizationTest.txt has all its cases (" + std::to_string(cases) + ")");
    // Part 1 lists every code point with a non-trivial normalization; all others are invariant.
    std::size_t invariant = 0;
    for (char32_t c = 0; c <= 0x10FFFF; ++c) {
        if ((c >= 0xD800 && c <= 0xDFFF) || part1.contains(c)) {
            continue;
        }
        const std::u32string single(1, c);
        check(unicode::to_nfc(single) == single, "NFC invariant: " + hex(single));
        ++invariant;
    }
    check(invariant > 1'000'000, "invariant code points checked");

    // Assigned / unassigned, combining classes and simple case folding spot checks.
    check(unicode::is_assigned(U'A') && unicode::is_assigned(0x4E00) &&
              unicode::is_assigned(0x9FFF),
          "assigned letters and CJK range");
    check(unicode::is_assigned(0xE000) && unicode::is_assigned(0xF0000) &&
              unicode::is_assigned(0x10FFFD),
          "private use is assigned");
    check(!unicode::is_assigned(0x0378) && !unicode::is_assigned(0xFDD0) &&
              !unicode::is_assigned(0xFFFE) && !unicode::is_assigned(0x10FFFF) &&
              !unicode::is_assigned(0xE0000),
          "unassigned code points and noncharacters");
    check(unicode::combining_class(0x0301) == 230 && unicode::combining_class(0x0323) == 220 &&
              unicode::combining_class(U'a') == 0,
          "combining classes");
    check(unicode::simple_fold(U'A') == U'a' && unicode::simple_fold(0x00C4) == 0x00E4 &&
              unicode::simple_fold(0x03C2) == 0x03C3 && unicode::simple_fold(0x10400) == 0x10428 &&
              unicode::simple_fold(0x212A) == U'k' && unicode::simple_fold(0x00DF) == 0x00DF &&
              unicode::simple_fold(0x1E9E) == 0x00DF && unicode::simple_fold(U'a') == U'a',
          "simple case folding (C + S, no F)");
    // simple_fold against CaseFolding.txt (status C and S) for every scalar value, so the ASCII
    // fast path and the table agree with the pinned data; every ASCII code point is assigned.
    {
        std::ifstream folding{std::filesystem::path{argv[1]}.parent_path() / "CaseFolding.txt"};
        check(folding.good(), "CaseFolding.txt opened");
        std::map<char32_t, char32_t> folds;
        for (std::string row; std::getline(folding, row);) {
            if (row.empty() || row[0] == '#') {
                continue;
            }
            const auto first = row.find(';'), second = row.find(';', first + 1);
            const auto status = row.substr(first + 2, 1);
            if (status == "C" || status == "S") {
                const auto from = static_cast<char32_t>(std::stoul(row.substr(0, first), nullptr, 16));
                const auto to =
                    static_cast<char32_t>(std::stoul(row.substr(second + 2), nullptr, 16));
                folds[from] = to;
            }
        }
        check(folds.size() > 1400, "CaseFolding.txt has its C and S mappings");
        std::size_t wrong = 0;
        for (char32_t c = 0; c <= 0x10FFFF; ++c) {
            const auto it = folds.find(c);
            wrong += unicode::simple_fold(c) != (it == folds.end() ? c : it->second) ? 1 : 0;
        }
        check(wrong == 0, "simple_fold equals CaseFolding.txt C+S everywhere (" +
                              std::to_string(wrong) + " differences)");
        bool ascii = true;
        for (char32_t c = 0; c < 0x80; ++c) {
            ascii = ascii && unicode::is_assigned(c);
        }
        check(ascii, "every ASCII code point is assigned");
    }
    // UTF-8 round trip of boundary scalar values.
    const std::u32string edges{0x7F, 0x80, 0x7FF, 0x800, 0xD7FF, 0xE000, 0xFFFD, 0x10000, 0x10FFFF};
    const auto decoded = unicode::decode_utf8(unicode::encode_utf8(edges));
    check(decoded && *decoded == edges, "UTF-8 encode / decode boundaries");
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
    }
    return failures == 0 ? 0 : 1;
}
