// SPDX-License-Identifier: GPL-3.0-or-later
#include "seedarc/archive_path.hpp"

#include "filesystem/unicode_profile.hpp"

#include <algorithm>
#include <map>
#include <ranges>

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
        if (unicode::encode_utf8(part).size() > 255 || parts.size() >= limits.max_components ||
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
    // only in case collide on every platform; '/' folds to itself.
    std::map<std::u32string, bool> names;
    std::size_t used = 0;
    const auto budget = (std::min)(limits.max_metadata_bytes, std::size_t{16 * 1024 * 1024});
    for (const auto &entry : entries) {
        if (used > budget || budget - used < 32 || entry.path.size() > budget - used - 32) {
            return std::unexpected(Error{ErrorCode::resource_limit, "manifest metadata limit"});
        }
        used += 32 + entry.path.size();
        const auto parsed = components_of(entry.path, limits);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        std::u32string key;
        for (const auto &component : *parsed) {
            if (!key.empty()) {
                key += U'/';
            }
            for (const auto c : component) {
                key += unicode::simple_fold(c);
            }
        }
        if (!names.emplace(std::move(key), entry.directory).second) {
            return std::unexpected(unsafe("duplicate or case-colliding archive path"));
        }
    }
    for (const auto &name : std::views::keys(names)) {
        for (auto slash = name.find(U'/'); slash != std::u32string::npos;
             slash = name.find(U'/', slash + 1)) {
            const auto ancestor = names.find(name.substr(0, slash));
            if (ancestor != names.end() && !ancestor->second) {
                return std::unexpected(unsafe("file is also used as an archive directory"));
            }
        }
    }
    return {};
}
} // namespace seedarc::experimental
