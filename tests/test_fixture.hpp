// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Fixture helpers shared by the tests (Windows and Linux). Some Windows hosts briefly list a
// foreign "<NAME>.tmp" entry (the deleted name upper-cased plus ".tmp") about a millisecond after
// a file is deleted; it can never be opened (ERROR_FILE_NOT_FOUND) and is not the deleted object.
// See STATUS DEV-011.

#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace test_fixture {
// "<process id>-<monotonic ticks>": a fixture directory name unique to this run.
inline std::string unique_suffix() {
#ifdef _WIN32
    const auto pid = static_cast<unsigned long long>(GetCurrentProcessId());
#else
    const auto pid = static_cast<unsigned long long>(::getpid());
#endif
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::to_string(pid) + "-" + std::to_string(ticks);
}

// True only for a name that is gone by the time it is opened. A real leftover, including one
// still delete-pending behind an open handle (ERROR_ACCESS_DENIED), is not "vanished".
inline bool vanished(const std::filesystem::path &path) {
#ifdef _WIN32
    const HANDLE handle = CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
        return false;
    }
    const auto error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
#else
    struct stat status{};
    return ::lstat(path.c_str(), &status) != 0 && (errno == ENOENT || errno == ENOTDIR);
#endif
}

// Removes a fixture tree the caller already checked for links. libstdc++'s remove_all() spins
// forever when an enumerated entry vanishes before it is removed
// (recursive_directory_iterator::__erase does not advance), so this does its own bounded walk:
// a vanished child counts as removed and a directory gets a few sleep-free re-listings.
// Directories are made writable first so that tests may leave read-only fixtures behind.
inline std::error_code remove_tree(const std::filesystem::path &root, int depth = 0) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (depth > 64) {
        return std::make_error_code(std::errc::too_many_links);
    }
#ifndef _WIN32
    fs::permissions(root, fs::perms::owner_all, fs::perm_options::add, ec);
    ec.clear();
#endif
    for (int pass = 0; pass != 3; ++pass) {
        std::error_code list;
        for (fs::directory_iterator it(root, list), end; !list && it != end; it.increment(list)) {
            const auto child = it->path();
            std::error_code type;
            const auto status = fs::symlink_status(child, type);
            if (type) {
                if (vanished(child)) {
                    continue;
                }
                return type;
            }
            if (fs::is_directory(status)) {
                if (const auto nested = remove_tree(child, depth + 1)) {
                    return nested;
                }
            } else if (fs::remove(child, ec); ec && !vanished(child)) {
                return ec;
            }
        }
        if (list && !vanished(root)) {
            return list;
        }
        if (fs::remove(root, ec); !ec || vanished(root)) {
            return {};
        }
    }
    return ec;
}

// True when every entry below `root` is a plain file or directory (no reparse point or symbolic
// link that a recursive removal could follow). A name gone by the time it is examined is not a
// risk.
inline bool reparse_free(const std::filesystem::path &root) {
    namespace fs = std::filesystem;
#ifdef _WIN32
    const auto attributes = GetFileAttributesW(root.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return false;
    }
#else
    std::error_code root_ec;
    if (fs::symlink_status(root, root_ec).type() != fs::file_type::directory || root_ec) {
        return false;
    }
#endif
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec)) {
        if (ec) {
            return false;
        }
#ifdef _WIN32
        const auto item = GetFileAttributesW(it->path().c_str());
        if (item == INVALID_FILE_ATTRIBUTES ? !vanished(it->path())
                                            : (item & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            return false;
        }
#else
        std::error_code type;
        const auto status = fs::symlink_status(it->path(), type);
        if (type ? !vanished(it->path()) : fs::is_symlink(status)) {
            return false;
        }
#endif
    }
    return !ec;
}
} // namespace test_fixture
