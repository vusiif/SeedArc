// SPDX-License-Identifier: GPL-3.0-or-later
#include "seedarc/error.hpp"
#include "seedarc/version.hpp"

#include <iostream>
#include <string_view>

namespace {
constexpr std::string_view help =
    "SeedArc - experimental compression project\n"
    "Usage: seedarc --help | --version\n"
    "  -h, --help     Show this help\n"
    "  --version      Show version\n"
    "Planned commands (not implemented): compress, extract, verify, search, benchmark\n";

int report_error(const seedarc::Error &error) {
    std::cerr << "seedarc: " << error.message << '\n';
    return error.code == seedarc::ErrorCode::not_implemented ? 3 : 2;
}
} // namespace

int main(int argc, char *argv[]) {
    if (argc < 2) {
        std::cerr << help;
        return report_error({seedarc::ErrorCode::invalid_argument, "a command is required"});
    }

    const std::string_view command{argv[1]};
    if (command == "compress" || command == "extract" || command == "verify" ||
        command == "search" || command == "benchmark") {
        return report_error({seedarc::ErrorCode::not_implemented,
                             "command is not implemented in M0: " + std::string{command}});
    }
    if (command != "--help" && command != "-h" && command != "help" && command != "--version") {
        return report_error({seedarc::ErrorCode::invalid_argument,
                             "unknown command or option: " + std::string{command}});
    }
    if (argc != 2) {
        return report_error({seedarc::ErrorCode::invalid_argument, "unexpected extra arguments"});
    }
    if (command == "--version") {
        std::cout << "SeedArc " << seedarc::version() << '\n';
    } else {
        std::cout << help;
    }
    return 0;
}
