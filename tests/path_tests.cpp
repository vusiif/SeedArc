// SPDX-License-Identifier: GPL-3.0-or-later
#include "seedarc/archive_path.hpp"

#include <array>
#include <iostream>

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
    for (const auto name : {"\xe4\xb8\xad\xe6\x96\x87.txt", "caf\xc3\xa9"}) {
#ifdef _WIN32
        check(validate_archive_path(name).has_value(), "valid NFC relative path");
#else
        // Only the ASCII subset of the path profile exists off Windows (Linux L1).
        const auto result = validate_archive_path(name);
        check(!result && result.error().code == seedarc::ErrorCode::unsupported_feature,
              "non-ASCII path explicitly unsupported off Windows");
#endif
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
