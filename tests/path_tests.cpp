// SPDX-License-Identifier: GPL-3.0-or-later
#include "seedarc/archive_path.hpp"

#include <array>
#include <iostream>
#include <string>

using namespace seedarc::experimental;

int main() {
    int failures = 0;
    const auto check = [&failures](bool condition, const char *description) {
        if (!condition) {
            std::cerr << "FAIL: " << description << '\n';
            ++failures;
        }
    };
    for (const auto name : {"file.txt", ".config", "folder/sub/file"}) {
        check(validate_archive_path(name).has_value(), "valid ASCII relative path");
    }
    // ADR-0005: the same answers on every platform.
    // CJK, NFC e-acute, the Hangul syllable GA, an emoji directory with an Arabic name.
    for (const auto name : {"\xe4\xb8\xad\xe6\x96\x87.txt", "caf\xc3\xa9", "\xea\xb0\x80",
                            "\xf0\x9f\x98\x80/\xd8\xb9\xd8\xb1\xd8\xa8\xd9\x8a"}) {
        check(validate_archive_path(name).has_value(), "valid NFC relative path");
    }
    const auto unsafe = [&](const char *name, const char *description) {
        const auto result = validate_archive_path(name);
        check(!result && result.error().code == seedarc::ErrorCode::unsafe_path, description);
    };
    unsafe("cafe\xcc\x81", "NFD (e + U+0301) is not NFC");
    unsafe("\xe1\x84\x80\xe1\x85\xa1", "conjoining jamo L+V compose to a syllable: not NFC");
    unsafe("\xe2\x84\xab", "U+212B ANGSTROM SIGN is a singleton: not NFC");
    unsafe("a\xcc\xa3\xcc\x82", "a + U+0323 + U+0302 composes to U+1EAD: not NFC");
    unsafe("x\xcd\xb8", "U+0378 is unassigned");
    unsafe("\xef\xb7\x90", "U+FDD0 is a noncharacter");
    unsafe("\xef\xa4\x80", "U+F900 CJK compatibility ideograph is a singleton: not NFC");
    check(validate_archive_path("\xf3\xb0\x80\x80x").has_value(), "plane 15 private use accepted");
    {
        std::string longest;
        for (int i = 0; i < 85; ++i) {
            longest += "\xe4\xb8\xad";
        }
        check(validate_archive_path(longest).has_value(), "255 UTF-8 bytes per component");
        const auto result = validate_archive_path(longest + "a");
        check(!result && result.error().code == seedarc::ErrorCode::resource_limit,
              "256 UTF-8 bytes per component is the limit");
    }
    // The manifest's ASCII fast path must give the same answer as the general path, error code
    // and message included, so its checks stay in step with components_of.
    {
        std::string long_part(255, 'a'), too_long_part(256, 'a'), deep;
        for (int i = 0; i < 64; ++i) {
            deep += (i == 0 ? "" : "/") + std::string{"d"};
        }
        const std::string too_deep = deep + "/d", too_long(4097, 'a');
        const std::array<std::string, 40> cases{
            "a", "A/B/c.txt", "dir/file", ".config", "x.y.z", "a b", " lead", "",
            "/abs", "trail/", "a//b", ".", "..", "a/./b", "a/../b", "end.", "end ",
            "a\\b", "c:x", "a<b", "a>b", "a\"b", "a|b", "a?b", "a*b", "a~b",
            std::string{"ctl\x01"}, std::string{"del\x7f"}, "CON", "con.txt", "Com1.tar.gz",
            "lpt9", "COM0", "nul  .x", "CLOCK$", "conout$/x", long_part, too_long_part,
            too_deep, too_long};
        for (const auto &name : cases) {
            const auto single = validate_archive_path(name);
            const std::array<PathEntry, 1> one{{{name, false}}};
            const auto manifest = validate_manifest_paths(one);
            const bool same =
                single.has_value() == manifest.has_value() &&
                (single || (single.error().code == manifest.error().code &&
                            single.error().message == manifest.error().message));
            check(same, ("ASCII fast path agrees with components_of: " + name).c_str());
        }
        check(validate_manifest_paths(std::array<PathEntry, 1>{{{deep, false}}}).has_value(),
              "64 components accepted");
        const std::array<PathEntry, 2> kelvin{{{"K", false}, {"\xe2\x84\xaa", false}}};
        check(!validate_manifest_paths(kelvin), "ASCII K collides with U+212A KELVIN SIGN");
        const std::array<PathEntry, 2> mixed{{{"Dir/x", false}, {"dir", false}}};
        check(!validate_manifest_paths(mixed), "ASCII case-folded file used as an ancestor");
    }
    {
        const std::array<PathEntry, 2> umlaut{{{"\xc3\x84", false}, {"\xc3\xa4", false}}};
        check(!validate_manifest_paths(umlaut), "U+00C4 / U+00E4 collide");
        const std::array<PathEntry, 2> sigma{{{"\xcf\x83", false}, {"\xcf\x82", false}}};
        check(!validate_manifest_paths(sigma), "sigma / final sigma collide");
        const std::array<PathEntry, 2> deseret{
            {{"\xf0\x90\x90\x80", false}, {"\xf0\x90\x90\xa8", false}}};
        check(!validate_manifest_paths(deseret), "supplementary U+10400 / U+10428 collide");
        const std::array<PathEntry, 2> sharp{{{"\xc3\x9f", false}, {"ss", false}}};
        check(validate_manifest_paths(sharp).has_value(),
              "simple folding keeps sharp s and ss apart");
        const std::array<PathEntry, 2> ancestor{{{"\xc3\x84/x", false}, {"\xc3\xa4", false}}};
        check(!validate_manifest_paths(ancestor), "case-folded file used as an ancestor");
    }
    for (const auto name : {"",
                            ".",
                            "..",
                            "a/../b",
                            "a/./b",
                            "/root",
                            "a/",
                            "a//b",
                            "C:/file",
                            "C:file",
                            "\\\\server\\share",
                            "\\\\?\\C:\\file",
                            "a\\b",
                            "a:stream",
                            "NUL.txt",
                            "con",
                            "AUX",
                            "COM1.bin",
                            "LPT9",
                            "COM\xc2\xb9",
                            "CONIN$",
                            "CONOUT$",
                            "a.",
                            "a ",
                            "bad?",
                            "a~1",
                            "\xc0\xaf",
                            "\xed\xa0\x80",
                            "\xf4\x90\x80\x80",
                            "cafe\xcc\x81"}) {
        check(!validate_archive_path(name), "reject malicious, malformed or ambiguous path");
    }
    // Malformed UTF-8 is an unsafe path on every platform (overlong, surrogate, > U+10FFFF,
    // truncated, stray continuation, invalid lead bytes).
    for (const auto name :
         {"\xc0\xaf", "\xc1\xbf", "\xe0\x80\xaf", "\xed\xa0\x80", "\xf0\x80\x80\xaf",
          "\xf4\x90\x80\x80", "\xf5\x80\x80\x80", "\xe4\xb8", "a\x80", "\xff", "ok/\xc3"}) {
        const auto result = validate_archive_path(name);
        check(!result && result.error().code == seedarc::ErrorCode::unsafe_path,
              "malformed UTF-8 rejected as unsafe path");
    }
    check(!validate_archive_path(std::string_view{"a\0b", 3}), "reject embedded NUL");
    check(!validate_archive_path(std::string(256, 'x')), "component size limit");
    check(!validate_archive_path(std::string(4097, 'x')), "path byte limit");
    std::string deep = "a";
    for (int i = 0; i < 64; ++i) {
        deep += "/a";
    }
    check(!validate_archive_path(deep), "component count limit");
    const std::array<PathEntry, 3> valid{{{"a", true}, {"a/b", false}, {"empty", true}}};
    check(validate_manifest_paths(valid).has_value(), "directory and child accepted");
    const std::array<PathEntry, 2> repeated{{{"A.txt", false}, {"a.txt", false}}};
    check(!validate_manifest_paths(repeated), "case collision");
    const std::array<PathEntry, 2> duplicate{{{"a", true}, {"a", false}}};
    check(!validate_manifest_paths(duplicate), "duplicate type conflict");
    const std::array<PathEntry, 2> prefix{{{"a/b/c", false}, {"A", false}}};
    check(!validate_manifest_paths(prefix), "file used as ancestor, independent of order");
    auto limits = PathLimits{};
    limits.max_entries = 1;
    check(!validate_manifest_paths(valid, limits), "manifest count limit");
    limits = {};
    limits.max_metadata_bytes = 33;
    check(!validate_manifest_paths(valid, limits), "aggregate metadata limit");
    return failures == 0 ? 0 : 1;
}
