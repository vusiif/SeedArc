// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Windows fixture helpers for the seed tests. Some hosts briefly list a foreign "<NAME>.tmp"
// entry (the deleted name upper-cased plus ".tmp") about a millisecond after a file is deleted;
// it can never be opened (ERROR_FILE_NOT_FOUND) and is not the deleted object. See STATUS DEV-011.

#include <filesystem>
#include <system_error>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace seed_test {
// True only for a name that is gone by the time it is opened. A real leftover, including one
// still delete-pending behind an open handle (ERROR_ACCESS_DENIED), is not "vanished".
inline bool vanished(const std::filesystem::path &path) {
    const HANDLE handle = CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
        return false;
    }
    const auto error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

// Removes a fixture tree the caller already checked for reparse points. libstdc++'s
// remove_all() spins forever when an enumerated entry vanishes before it is removed
// (recursive_directory_iterator::__erase does not advance), so this does its own bounded walk:
// a vanished child counts as removed and a directory gets a few sleep-free re-listings.
inline std::error_code remove_tree(const std::filesystem::path &root, int depth = 0) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (depth > 64) {
        return std::make_error_code(std::errc::too_many_links);
    }
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
} // namespace seed_test
