// SPDX-License-Identifier: GPL-3.0-or-later
#include "seedarc/archive_path.hpp"

#include "filesystem/unicode_profile.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <ranges>
#include <string_view>

namespace seedarc::experimental {
namespace {
namespace unicode = internal::unicode;

Error unsafe(std::string message) { return {ErrorCode::unsafe_path, std::move(message)}; }

// Windows device names (ADR-0001), compared after ASCII uppercasing of the part before the
// first dot, with trailing spaces removed.
bool reserved_name(std::u32string_view component) {
    auto base = component.substr(0, component.find(U'.'));
    while (!base.empty() && base.back() == U' ') {
        base.remove_suffix(1);
    }
    if (base.size() < 3 || base.size() > 7) {
        return false; // every reserved name has 3 to 7 characters
    }
    std::u32string upper{base};
    for (auto &c : upper) {
        if (c >= U'a' && c <= U'z') {
            c = c - U'a' + U'A';
        }
    }
    if (upper == U"CON" || upper == U"PRN" || upper == U"AUX" || upper == U"NUL" ||
        upper == U"CLOCK$" || upper == U"CONIN$" || upper == U"CONOUT$") {
        return true;
    }
    return upper.size() == 4 && (upper.starts_with(U"COM") || upper.starts_with(U"LPT")) &&
           ((upper[3] >= U'1' && upper[3] <= U'9') || upper[3] == U'\u00b9' ||
            upper[3] == U'\u00b2' || upper[3] == U'\u00b3');
}

// The platform's wide form of a validated component: UTF-16 on Windows (NT names), UTF-32
// where wchar_t holds a full scalar value (Linux; callers there use the original UTF-8 bytes).
std::wstring to_wide(std::u32string_view component) {
    std::wstring wide;
    wide.reserve(component.size());
    for (const auto c : component) {
        if constexpr (sizeof(wchar_t) == 2) {
            if (c >= 0x10000) {
                const auto v = c - 0x10000;
                wide.push_back(static_cast<wchar_t>(0xd800 + (v >> 10)));
                wide.push_back(static_cast<wchar_t>(0xdc00 + (v & 0x3ff)));
                continue;
            }
        }
        wide.push_back(static_cast<wchar_t>(c));
    }
    return wide;
}

// Archive path profile (ADR-0005): strict UTF-8, every code point assigned in the pinned
// Unicode version, and already in NFC. Paths are never silently renamed.
std::expected<std::u32string, Error> decode_path(std::string_view path) {
    auto decoded = unicode::decode_utf8(path);
    if (!decoded) {
        return std::unexpected(unsafe("archive path is not valid UTF-8"));
    }
    if (!std::ranges::all_of(*decoded, unicode::is_assigned)) {
        return std::unexpected(unsafe("archive path contains a code point unassigned in Unicode " +
                                      std::string{unicode::ucd_version()}));
    }
    if (!unicode::is_nfc(*decoded)) {
        return std::unexpected(unsafe("archive path must already be NFC normalized"));
    }
    return std::move(*decoded);
}

// Splits a decoded path into checked components.
std::expected<std::vector<std::u32string>, Error> components_of(std::string_view path,
                                                                const PathLimits &limits) {
    if (path.empty()) {
        return std::unexpected(unsafe("empty archive path"));
    }
    if (path.size() > limits.max_path_bytes || path.size() > 4096) {
        return std::unexpected(Error{ErrorCode::resource_limit, "archive path exceeds byte limit"});
    }
    const auto decoded = decode_path(path);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    const std::u32string &text = *decoded;
    for (const auto c : text) {
        if (c < 32 || c == 127 ||
            std::u32string_view{U"\\:<>\"|?*~"}.find(c) != std::u32string_view::npos) {
            return std::unexpected(unsafe("archive path contains a prohibited character"));
        }
    }
    std::vector<std::u32string> parts;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        const auto slash = text.find(U'/', begin);
        const auto end = slash == std::u32string::npos ? text.size() : slash;
        const auto part = std::u32string_view{text}.substr(begin, end - begin);
        if (part.empty() || part == U"." || part == U".." || part.back() == U'.' ||
            part.back() == U' ' || reserved_name(part)) {
            return std::unexpected(unsafe("archive path contains an unsafe component"));
        }
        // 255 UTF-8 bytes: the Linux NAME_MAX, and at most 255 UTF-16 units on Windows.
        std::size_t utf8_bytes = 0;
        for (const auto c : part) {
            utf8_bytes += c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
        }
        if (utf8_bytes > 255 || parts.size() >= limits.max_components ||
            parts.size() >= 64) {
            return std::unexpected(
                Error{ErrorCode::resource_limit, "archive path component limit"});
        }
        parts.emplace_back(part);
        if (slash == std::u32string::npos) {
            break;
        }
        begin = slash + 1;
    }
    return parts;
}

// The manifest key of a path: its checked components, simple-case-folded, joined by '/', as
// UTF-8 (one-to-one, so keys collide exactly when the folded code points do). An all-ASCII path
// takes a byte-level route that applies components_of's checks in the same order with the same
// errors (ASCII is assigned, NFC, and folds exactly A-Z); anything else goes through
// components_of. tests/path_tests.cpp compares both routes.
std::expected<std::string, Error> folded_key(std::string_view path, const PathLimits &limits) {
    if (!std::ranges::all_of(path, [](char c) { return static_cast<unsigned char>(c) < 0x80; })) {
        auto parts = components_of(path, limits);
        if (!parts) {
            return std::unexpected(parts.error());
        }
        std::u32string key;
        for (const auto &component : *parts) {
            if (!key.empty()) {
                key += U'/';
            }
            for (const auto c : component) {
                key += unicode::simple_fold(c);
            }
        }
        return unicode::encode_utf8(key);
    }
    if (path.empty()) {
        return std::unexpected(unsafe("empty archive path"));
    }
    if (path.size() > limits.max_path_bytes || path.size() > 4096) {
        return std::unexpected(Error{ErrorCode::resource_limit, "archive path exceeds byte limit"});
    }
    for (const auto c : path) {
        if (static_cast<unsigned char>(c) < 32 || c == 127 ||
            std::string_view{"\\:<>\"|?*~"}.find(c) != std::string_view::npos) {
            return std::unexpected(unsafe("archive path contains a prohibited character"));
        }
    }
    std::string key;
    key.reserve(path.size());
    std::size_t begin = 0, count = 0;
    while (begin <= path.size()) {
        const auto slash = path.find('/', begin);
        const auto end = slash == std::string_view::npos ? path.size() : slash;
        const auto part = path.substr(begin, end - begin);
        // reserved_name only looks at the part before the first dot, without trailing spaces,
        // and only 3 to 7 characters can match: widen just that.
        auto base = part.substr(0, part.find('.'));
        while (!base.empty() && base.back() == ' ') {
            base.remove_suffix(1);
        }
        const bool reserved = base.size() >= 3 && base.size() <= 7 &&
                              reserved_name(std::u32string{base.begin(), base.end()});
        if (part.empty() || part == "." || part == ".." || part.back() == '.' ||
            part.back() == ' ' || reserved) {
            return std::unexpected(unsafe("archive path contains an unsafe component"));
        }
        if (part.size() > 255 || count >= limits.max_components || count >= 64) {
            return std::unexpected(
                Error{ErrorCode::resource_limit, "archive path component limit"});
        }
        ++count;
        if (!key.empty()) {
            key += '/';
        }
        for (const auto c : part) {
            key += c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        }
        if (slash == std::string_view::npos) {
            break;
        }
        begin = slash + 1;
    }
    return key;
}
} // namespace

std::expected<std::vector<std::wstring>, Error> validate_archive_path(std::string_view path,
                                                                      const PathLimits &limits) {
    auto parts = components_of(path, limits);
    if (!parts) {
        return std::unexpected(parts.error());
    }
    std::vector<std::wstring> wide;
    wide.reserve(parts->size());
    for (const auto &part : *parts) {
        wide.push_back(to_wide(part));
    }
    return wide;
}

std::expected<void, Error> validate_manifest_paths(std::span<const PathEntry> entries,
                                                   const PathLimits &limits) {
    if (entries.size() > limits.max_entries || entries.size() > 100000) {
        return std::unexpected(Error{ErrorCode::resource_limit, "manifest entry limit"});
    }
    // Keyed by the simple case folding of every code point (ADR-0005), so that names differing
    // only in case collide on every platform; '/' folds to itself. Transparent comparison:
    // ancestor lookups by view, without substring copies.
    std::map<std::string, bool, std::less<>> names;
    std::size_t used = 0;
    const auto budget = (std::min)(limits.max_metadata_bytes, std::size_t{16 * 1024 * 1024});
    for (const auto &entry : entries) {
        if (used > budget || budget - used < 32 || entry.path.size() > budget - used - 32) {
            return std::unexpected(Error{ErrorCode::resource_limit, "manifest metadata limit"});
        }
        used += 32 + entry.path.size();
        auto key = folded_key(entry.path, limits);
        if (!key) {
            return std::unexpected(key.error());
        }
        if (!names.emplace(std::move(*key), entry.directory).second) {
            return std::unexpected(unsafe("duplicate or case-colliding archive path"));
        }
    }
    // A file F is used as a directory exactly when some key starts with F + '/'. Keys with that
    // prefix are contiguous in the sorted map and start at lower_bound(F + '/'): one lookup per
    // file instead of one per ancestor of every name.
    std::string probe;
    for (const auto &[name, directory] : names) {
        if (directory) {
            continue;
        }
        probe.assign(name);
        probe += '/';
        const auto it = names.lower_bound(probe);
        if (it != names.end() && it->first.starts_with(probe)) {
            return std::unexpected(unsafe("file is also used as an archive directory"));
        }
    }
    return {};
}
} // namespace seedarc::experimental
