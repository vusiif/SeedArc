// SPDX-License-Identifier: GPL-3.0-or-later
// Fuzz target: archive path validation and manifest collision checks (Windows profile).
#include "seedarc/archive_path.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

using namespace seedarc::experimental;

namespace {
void require(bool condition) {
    if (!condition) {
        std::abort();
    }
}

// Property: every accepted path splits into bounded, non-special components without separators.
void check_path(std::string_view path) {
    const auto parts = validate_archive_path(path);
    if (!parts) {
        return;
    }
    require(!parts->empty() && parts->size() <= 64);
    for (const auto &part : *parts) {
        require(!part.empty() && part.size() <= 255 && part != L"." && part != L"..");
        require(part.find_first_of(L"/\\:") == std::wstring::npos);
        require(part.find(L'\0') == std::wstring::npos);
        require(part.back() != L'.' && part.back() != L' ');
    }
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    const std::string_view input{reinterpret_cast<const char *>(data), size};
    check_path(input);
    // Newline-separated manifest; a trailing '/' marks a directory entry.
    std::vector<PathEntry> entries;
    std::size_t begin = 0;
    while (begin <= input.size() && entries.size() < 512) {
        const auto end = input.find('\n', begin);
        auto line = input.substr(begin, end == std::string_view::npos ? std::string_view::npos
                                                                      : end - begin);
        PathEntry entry;
        if (!line.empty() && line.back() == '/') {
            entry.directory = true;
            line.remove_suffix(1);
        }
        entry.path = std::string{line};
        check_path(entry.path);
        entries.push_back(std::move(entry));
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1;
    }
    // Property: an accepted manifest only contains individually valid paths.
    if (validate_manifest_paths(entries)) {
        for (const auto &entry : entries) {
            require(validate_archive_path(entry.path).has_value());
        }
    }
    return 0;
}
