// SPDX-License-Identifier: GPL-3.0-or-later
// ADR-0005 evidence (Windows only): where the portable path profile (pinned UCD) and the Win32
// APIs it replaced (IsNormalizedString(NormalizationC), CompareStringOrdinal(ignore case)) give
// different answers on this machine. Prints counts and the first examples; asserts nothing.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "filesystem/unicode_profile.hpp"

#include <cstdio>
#include <string>

namespace unicode = seedarc::experimental::internal::unicode;

namespace {
std::wstring utf16(std::u32string_view text) {
    std::wstring out;
    for (const auto c : text) {
        if (c >= 0x10000) {
            out.push_back(static_cast<wchar_t>(0xd800 + ((c - 0x10000) >> 10)));
            out.push_back(static_cast<wchar_t>(0xdc00 + ((c - 0x10000) & 0x3ff)));
        } else {
            out.push_back(static_cast<wchar_t>(c));
        }
    }
    return out;
}

bool win_nfc(std::u32string_view text) {
    const auto wide = utf16(text);
    SetLastError(ERROR_SUCCESS);
    return IsNormalizedString(NormalizationC, wide.c_str(), static_cast<int>(wide.size())) != FALSE;
}

bool win_equal_ignore_case(char32_t a, char32_t b) {
    const auto x = utf16(std::u32string(1, a));
    const auto y = utf16(std::u32string(1, b));
    return CompareStringOrdinal(x.c_str(), static_cast<int>(x.size()), y.c_str(),
                                static_cast<int>(y.size()), TRUE) == CSTR_EQUAL;
}

void report(const char *what, unsigned long count, const std::string &examples) {
    std::printf("%-58s %8lu  %s\n", what, count, examples.c_str());
}

void note(std::string &examples, int &shown, char32_t c) {
    if (shown++ < 8) {
        char buffer[16];
        std::snprintf(buffer, sizeof(buffer), "U+%04X ", static_cast<unsigned>(c));
        examples += buffer;
    }
}
} // namespace

int main() {
    std::printf("profile UCD %s\n", std::string{unicode::ucd_version()}.c_str());

    // 1. NFC of every single scalar value, and of each value followed by U+0301 (combining).
    unsigned long nfc_single = 0;
    unsigned long nfc_pair = 0;
    unsigned long nfc_pair_private = 0;
    unsigned long unassigned_win_nfc = 0;
    std::string single_examples;
    std::string pair_examples;
    int single_shown = 0;
    int pair_shown = 0;
    for (char32_t c = 1; c <= 0x10FFFF; ++c) {
        if (c >= 0xD800 && c <= 0xDFFF) {
            continue;
        }
        const std::u32string one(1, c);
        if (!unicode::is_assigned(c)) {
            unassigned_win_nfc += win_nfc(one) ? 1 : 0;
            continue;
        }
        if (unicode::is_nfc(one) != win_nfc(one)) {
            ++nfc_single;
            note(single_examples, single_shown, c);
        }
        const std::u32string two{c, 0x0301};
        if (unicode::is_nfc(two) != win_nfc(two)) {
            const bool private_use =
                (c >= 0xE000 && c <= 0xF8FF) || c >= 0xF0000; // planes 15-16 are all private use
            if (private_use) {
                ++nfc_pair_private;
            } else {
                ++nfc_pair;
                note(pair_examples, pair_shown, c);
                if (pair_shown <= 8) {
                    pair_examples += unicode::is_nfc(two) ? "(profile NFC) " : "(Windows NFC) ";
                }
            }
        }
    }
    report("assigned: is_nfc(c) differs from IsNormalizedString", nfc_single, single_examples);
    report("assigned, not private use: is_nfc(c U+0301) differs", nfc_pair, pair_examples);
    report("private use: is_nfc(c U+0301) differs", nfc_pair_private, "");
    report("unassigned in profile, but Windows calls the text NFC", unassigned_win_nfc,
           "(profile rejects them all)");

    // 2. Case: profile collision (same simple fold) vs CompareStringOrdinal equality, for each
    // assigned value c against its fold and against its Windows-uppercase partner.
    unsigned long profile_only = 0;
    unsigned long windows_only = 0;
    std::string profile_examples;
    std::string windows_examples;
    int profile_shown = 0;
    int windows_shown = 0;
    for (char32_t c = 1; c <= 0x10FFFF; ++c) {
        if ((c >= 0xD800 && c <= 0xDFFF) || !unicode::is_assigned(c)) {
            continue;
        }
        const auto folded = unicode::simple_fold(c);
        if (folded != c && !win_equal_ignore_case(c, folded)) {
            ++profile_only;
            note(profile_examples, profile_shown, c);
        }
        if (c <= 0xFFFF) {
            wchar_t upper = static_cast<wchar_t>(c);
            CharUpperBuffW(&upper, 1);
            const auto partner = static_cast<char32_t>(upper);
            if (partner != c && win_equal_ignore_case(c, partner) &&
                unicode::simple_fold(c) != unicode::simple_fold(partner)) {
                ++windows_only;
                note(windows_examples, windows_shown, c);
            }
        }
    }
    report("collide in profile, not in CompareStringOrdinal", profile_only, profile_examples);
    report("collide in CompareStringOrdinal (BMP upper), not in profile", windows_only,
           windows_examples);
    return 0;
}
