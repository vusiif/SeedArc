// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Helpers shared by the research container tools (dict_study, DEV-013; g5_study, DEV-016):
// timing, peak memory, JSON strings, bounded input reading and little-endian fields. Research
// code only; nothing here is part of the SeedArc archive format.

#include "seedarc/error.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <psapi.h>
#else
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace seedarc::research_tools {
namespace fs = std::filesystem;
using seedarc::Error;
using seedarc::ErrorCode;

inline constexpr std::uint32_t max_files = 200000;
inline constexpr std::size_t max_path = 4096;
inline constexpr std::uint64_t max_input = 1ULL << 30;

// ---- measurement -------------------------------------------------------------------------
inline double wall_ms() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
inline double cpu_ms() {
#ifdef _WIN32
    FILETIME c{}, e{}, k{}, u{};
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) {
        return -1;
    }
    const auto ticks = [](const FILETIME &t) {
        return (static_cast<std::uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
    };
    return static_cast<double>(ticks(k) + ticks(u)) / 10000.0;
#else
    rusage r{};
    if (getrusage(RUSAGE_SELF, &r) != 0) {
        return -1;
    }
    return (r.ru_utime.tv_sec + r.ru_stime.tv_sec) * 1000.0 +
           (r.ru_utime.tv_usec + r.ru_stime.tv_usec) / 1000.0;
#endif
}
inline std::string peak_memory_json() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS m{};
    m.cb = sizeof(m);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &m, sizeof(m))) {
        return "\"peak_memory_bytes\":" + std::to_string(m.PeakWorkingSetSize) +
               ",\"peak_memory_metric\":\"PeakWorkingSetSize\"";
    }
#else
    rusage r{};
    if (getrusage(RUSAGE_SELF, &r) == 0) {
        return "\"peak_memory_bytes\":" +
               std::to_string(static_cast<std::uint64_t>(r.ru_maxrss) * 1024) +
               ",\"peak_memory_metric\":\"ru_maxrss\"";
    }
#endif
    return "\"peak_memory_bytes\":null,\"peak_memory_metric\":null";
}
// DEV-039 / ADR-0009: installed physical memory and the current resident set, for the
// `--jobs auto` worker budget. nullopt when the platform cannot tell.
inline std::optional<std::uint64_t> physical_memory_bytes() {
#ifdef _WIN32
    MEMORYSTATUSEX s{};
    s.dwLength = sizeof(s);
    if (GlobalMemoryStatusEx(&s)) {
        return s.ullTotalPhys;
    }
#else
    const auto pages = sysconf(_SC_PHYS_PAGES), page = sysconf(_SC_PAGESIZE);
    if (pages > 0 && page > 0) {
        return static_cast<std::uint64_t>(pages) * static_cast<std::uint64_t>(page);
    }
#endif
    return std::nullopt;
}
inline std::optional<std::uint64_t> resident_bytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS m{};
    m.cb = sizeof(m);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &m, sizeof(m))) {
        return m.WorkingSetSize;
    }
#else
    std::ifstream statm("/proc/self/statm");
    std::uint64_t size = 0, resident = 0;
    const auto page = sysconf(_SC_PAGESIZE);
    if (statm >> size >> resident && page > 0) {
        return resident * static_cast<std::uint64_t>(page);
    }
#endif
    return std::nullopt;
}
inline std::string json_string(std::string_view value) {
    std::string out = "\"";
    for (const auto raw : value) {
        const auto c = static_cast<unsigned char>(raw);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += raw;
        } else if (c < 32) {
            constexpr char hex[] = "0123456789abcdef";
            out += "\\u00";
            out += hex[c >> 4];
            out += hex[c & 15];
        } else {
            out += raw;
        }
    }
    return out + "\"";
}

// ---- input --------------------------------------------------------------------------------
struct InputFile {
    std::string path; // UTF-8, '/' separated, relative
    std::vector<std::byte> data;
};
inline fs::path native(std::string_view utf8) {
    return fs::path{std::u8string{utf8.begin(), utf8.end()}};
}
inline std::string utf8(const fs::path &p) {
    const auto u = p.generic_u8string();
    return {u.begin(), u.end()};
}
inline std::expected<std::vector<std::byte>, Error> read_file(const fs::path &path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return std::unexpected(Error{ErrorCode::io_error, "cannot open " + utf8(path)});
    }
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec || size > max_input) {
        return std::unexpected(Error{ErrorCode::resource_limit, "input file too large"});
    }
    std::vector<std::byte> data(static_cast<std::size_t>(size));
    if (!stream.read(reinterpret_cast<char *>(data.data()),
                     static_cast<std::streamsize>(data.size()))) {
        return std::unexpected(Error{ErrorCode::io_error, "cannot read " + utf8(path)});
    }
    return data;
}
inline std::expected<std::vector<InputFile>, Error> read_tree(const fs::path &root) {
    std::vector<InputFile> files;
    std::uint64_t total = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec)) {
        if (ec) {
            return std::unexpected(Error{ErrorCode::io_error, "cannot enumerate input"});
        }
        if (it->is_symlink() || !it->is_regular_file()) {
            continue;
        }
        auto data = read_file(it->path());
        if (!data) {
            return std::unexpected(data.error());
        }
        total += data->size();
        if (files.size() == max_files || total > max_input) {
            return std::unexpected(Error{ErrorCode::resource_limit, "input too large"});
        }
        files.push_back({utf8(fs::relative(it->path(), root)), std::move(*data)});
    }
    std::ranges::sort(files, {}, &InputFile::path);
    return files;
}
inline std::uint64_t fnv1a(std::string_view text) {
    std::uint64_t h = 1469598103934665603ULL;
    for (const auto c : text) {
        h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ULL;
    }
    return h;
}

// ---- little-endian fields -------------------------------------------------------------------
inline void put(std::vector<std::byte> &out, std::uint64_t value, int bytes) {
    for (int i = 0; i < bytes; ++i) {
        out.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xff));
    }
}
struct Reader {
    std::span<const std::byte> bytes;
    std::size_t at = 0;
    std::optional<std::uint64_t> get(int n) {
        if (bytes.size() - at < static_cast<std::size_t>(n)) {
            return std::nullopt;
        }
        std::uint64_t v = 0;
        for (int i = 0; i < n; ++i) {
            v |= std::uint64_t{std::to_integer<unsigned>(bytes[at + i])} << (8 * i);
        }
        at += static_cast<std::size_t>(n);
        return v;
    }
};
inline std::optional<std::uint64_t> number(std::string_view text) {
    std::uint64_t v = 0;
    if (text.empty() || text.size() > 12) {
        return std::nullopt;
    }
    for (const auto c : text) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        v = v * 10 + static_cast<std::uint64_t>(c - '0');
    }
    return v;
}
} // namespace seedarc::research_tools
