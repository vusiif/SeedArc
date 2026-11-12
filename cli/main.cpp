// SPDX-License-Identifier: GPL-3.0-or-later
#include "seedarc/error.hpp"
#include "seedarc/store_archive.hpp"
#include "seedarc/version.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <shellapi.h>
#endif

namespace {
namespace fs = std::filesystem;
using seedarc::Error;
using seedarc::ErrorCode;

constexpr std::string_view help =
    "SeedArc - experimental compression project\n"
    "Usage:\n"
    "  seedarc compress <file-or-directory> -o <archive>\n"
    "  seedarc verify <archive>\n"
    "  seedarc extract <archive> -o <new-directory>\n"
    "  seedarc --help | --version\n"
    "\n"
    "The archive format is experimental and may change; archives currently store data\n"
    "uncompressed (Store). Outputs are never overwritten: the archive and the extraction\n"
    "directory must not exist yet, and their parent directory must be on a local NTFS volume.\n"
    "Symbolic links and junctions in the input are rejected. Work happens in a temporary\n"
    "named .seedarc-<hex>.tmp next to the output; one may remain only if the process is\n"
    "killed and can then be deleted.\n"
    "\n"
    "Exit codes: 0 success, 1 damaged or invalid archive, 2 usage error, 3 not implemented,\n"
    "4 cancelled, 5 I/O, conflict, limit or path error.\n"
    "Planned commands (not implemented): search, benchmark\n";

// Command line arguments stay in the native encoding so that non-ASCII paths survive.
using Argument = fs::path::string_type;

bool is(const Argument &argument, std::string_view text) {
    return std::ranges::equal(argument, text, [](Argument::value_type left, char right) {
        return left == static_cast<Argument::value_type>(static_cast<unsigned char>(right));
    });
}

// For echoing user input in messages; anything outside printable ASCII becomes '?'.
std::string printable(const Argument &argument) {
    std::string text;
    for (const auto c : argument) {
        text += c >= 0x20 && c < 0x7f ? static_cast<char>(c) : '?';
    }
    return text;
}

int exit_code(ErrorCode code) {
    switch (code) {
    case ErrorCode::malformed_archive:
    case ErrorCode::integrity_error:
    case ErrorCode::arithmetic_overflow:
        return 1;
    case ErrorCode::invalid_argument:
        return 2;
    case ErrorCode::not_implemented:
        return 3;
    case ErrorCode::cancelled:
        return 4;
    case ErrorCode::unsupported_feature:
    case ErrorCode::resource_limit:
    case ErrorCode::unsafe_path:
    case ErrorCode::io_error:
    case ErrorCode::output_conflict:
        return 5;
    }
    return 5;
}

int report_error(const Error &error) {
    std::cerr << "seedarc: " << error.message << '\n';
    return exit_code(error.code);
}

int usage_error(std::string message) {
    return report_error({ErrorCode::invalid_argument, std::move(message)});
}

std::stop_source &cancellation() {
    static std::stop_source source;
    return source;
}

#ifdef _WIN32
BOOL WINAPI on_console_event(DWORD) {
    cancellation().request_stop(); // the running operation cleans up and exits with code 4
    return TRUE;
}
#endif

struct Operands {
    std::vector<fs::path> positional;
    std::optional<fs::path> output;
};

std::optional<Operands> parse_operands(const std::vector<Argument> &arguments, std::string &error) {
    Operands operands;
    for (std::size_t i = 2; i < arguments.size(); ++i) {
        if (is(arguments[i], "-o") || is(arguments[i], "--output")) {
            if (operands.output || i + 1 >= arguments.size()) {
                error = "-o requires exactly one path";
                return std::nullopt;
            }
            operands.output = fs::path{arguments[++i]};
        } else if (!arguments[i].empty() && arguments[i].front() == '-') {
            error = "unknown option: " + printable(arguments[i]);
            return std::nullopt;
        } else {
            operands.positional.emplace_back(arguments[i]);
        }
    }
    return operands;
}

void print_summary(std::string_view verb, const seedarc::experimental::ArchiveSummary &summary) {
    std::cout << verb << ": " << summary.entries << " entries, " << summary.total_output
              << " content bytes, " << summary.blocks << " blocks, archive "
              << summary.archive_bytes << " bytes\n";
}

int run(const std::vector<Argument> &arguments) {
    using namespace seedarc::experimental;
    if (arguments.size() < 2) {
        std::cerr << help;
        return usage_error("a command is required");
    }
    const auto &command = arguments[1];
    if (is(command, "search") || is(command, "benchmark")) {
        return report_error(
            {ErrorCode::not_implemented, "command is not implemented yet: " + printable(command)});
    }
    if (is(command, "--help") || is(command, "-h") || is(command, "help") ||
        is(command, "--version")) {
        if (arguments.size() != 2) {
            return usage_error("unexpected extra arguments");
        }
        if (is(command, "--version")) {
            std::cout << "SeedArc " << seedarc::version() << '\n';
        } else {
            std::cout << help;
        }
        return 0;
    }
    const bool compress = is(command, "compress");
    const bool extract = is(command, "extract");
    if (!compress && !extract && !is(command, "verify")) {
        return usage_error("unknown command or option: " + printable(command));
    }
    std::string problem;
    const auto operands = parse_operands(arguments, problem);
    if (!operands) {
        return usage_error(std::move(problem));
    }
    if (operands->positional.size() != 1) {
        return usage_error(printable(command) + " requires exactly one input path");
    }
    if ((compress || extract) != operands->output.has_value()) {
        return usage_error(compress || extract ? printable(command) + " requires -o <path>"
                                               : "verify does not take -o");
    }
#ifdef _WIN32
    SetConsoleCtrlHandler(on_console_event, TRUE);
#endif
    const auto stop = cancellation().get_token();
    const auto &input = operands->positional.front();
    const auto result = compress  ? compress_store(input, *operands->output, {}, stop)
                        : extract ? extract_archive(input, *operands->output, {}, stop)
                                  : verify_archive(input, {}, stop);
    if (!result) {
        return report_error(result.error());
    }
    print_summary(compress ? "compressed" : extract ? "extracted" : "verified", *result);
    return 0;
}
} // namespace

int main(int argc, char *argv[]) {
    std::vector<Argument> arguments;
#ifdef _WIN32
    (void)argc;
    (void)argv;
    int count = 0;
    wchar_t **wide = CommandLineToArgvW(GetCommandLineW(), &count);
    if (wide == nullptr) {
        return report_error({ErrorCode::io_error, "cannot read the command line"});
    }
    arguments.assign(wide, wide + count);
    LocalFree(wide);
#else
    arguments.assign(argv, argv + argc);
#endif
    return run(arguments);
}
