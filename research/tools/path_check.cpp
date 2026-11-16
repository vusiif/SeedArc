// SPDX-License-Identifier: GPL-3.0-or-later
// Research helper (DEV-016): checks candidate archive paths with the production path profile
// (validate_archive_path, ADR-0001 / ADR-0005), so corpus builders filter exactly what the
// archiver would refuse. Reads UTF-8 paths, one per line ('/' separators), from stdin and
// writes "ok\t<path>" or "<error code>\t<path>\t<message>" for each. Exit 0 unless input fails.
#include "seedarc/archive_path.hpp"

#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {
const char *code_name(seedarc::ErrorCode code) {
    using seedarc::ErrorCode;
    switch (code) {
    case ErrorCode::unsafe_path:
        return "unsafe_path";
    case ErrorCode::resource_limit:
        return "resource_limit";
    case ErrorCode::unsupported_feature:
        return "unsupported_feature";
    case ErrorCode::invalid_argument:
        return "invalid_argument";
    default:
        return "other_error";
    }
}
} // namespace

int main(int argc, char *argv[]) {
    std::ios::sync_with_stdio(false);
    const bool manifest = argc == 2 && std::string_view{argv[1]} == "--manifest";
    if (argc > 1 && !manifest) {
        std::cerr << "usage: seedarc_path_check [--manifest] < paths\n";
        return 2;
    }
    std::string line;
    if (manifest) {
        // The whole list as one archive manifest (files only): the production duplicate,
        // case-collision and file-as-ancestor checks. Prints "ok" or "<code>\t<message>".
        std::vector<seedarc::experimental::PathEntry> entries;
        while (std::getline(std::cin, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            entries.push_back({line, false});
        }
        // A real archive also lists every parent directory, and "a" / "A" directories collide.
        std::set<std::string> directories;
        for (const auto &e : entries) {
            for (auto slash = e.path.find('/'); slash != std::string::npos;
                 slash = e.path.find('/', slash + 1)) {
                directories.insert(e.path.substr(0, slash));
            }
        }
        for (const auto &d : directories) {
            entries.push_back({d, true});
        }
        seedarc::experimental::PathLimits limits;
        limits.max_entries = entries.size();
        limits.max_metadata_bytes = 16 * 1024 * 1024;
        if (const auto result = seedarc::experimental::validate_manifest_paths(entries, limits);
            result) {
            std::cout << "ok\t" << entries.size() << '\n';
        } else {
            std::cout << code_name(result.error().code) << '\t' << result.error().message << '\n';
        }
        return std::cin.bad() ? 1 : 0;
    }
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (const auto result = seedarc::experimental::validate_archive_path(line); result) {
            std::cout << "ok\t" << line << '\n';
        } else {
            std::cout << code_name(result.error().code) << '\t' << line << '\t'
                      << result.error().message << '\n';
        }
    }
    return std::cin.bad() ? 1 : 0;
}
