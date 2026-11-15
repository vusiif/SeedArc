// SPDX-License-Identifier: GPL-3.0-or-later
// Research helper (DEV-016): checks candidate archive paths with the production path profile
// (validate_archive_path, ADR-0001 / ADR-0005), so corpus builders filter exactly what the
// archiver would refuse. Reads UTF-8 paths, one per line ('/' separators), from stdin and
// writes "ok\t<path>" or "<error code>\t<path>\t<message>" for each. Exit 0 unless input fails.
#include "seedarc/archive_path.hpp"

#include <iostream>
#include <string>

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

int main() {
    std::ios::sync_with_stdio(false);
    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const auto result = seedarc::experimental::validate_archive_path(line);
        if (result) {
            std::cout << "ok\t" << line << '\n';
        } else {
            std::cout << code_name(result.error().code) << '\t' << line << '\t'
                      << result.error().message << '\n';
        }
    }
    return std::cin.bad() ? 1 : 0;
}
