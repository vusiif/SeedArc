// SPDX-License-Identifier: GPL-3.0-or-later
#include "seedarc/archive_path.hpp"

#include <algorithm>
#include <map>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace seedarc::experimental {
namespace {
Error unsafe(std::string message) { return {ErrorCode::unsafe_path, std::move(message)}; }

bool reserved_name(std::wstring_view component) {
    auto base = component.substr(0, component.find(L'.'));
    while (!base.empty() && base.back() == L' ') {
        base.remove_suffix(1);
    }
    std::wstring upper{base};
    for (auto &c : upper) {
        if (c >= L'a' && c <= L'z') {
            c = static_cast<wchar_t>(c - L'a' + L'A');
        }
    }
    if (upper == L"CON" || upper == L"PRN" || upper == L"AUX" || upper == L"NUL" ||
        upper == L"CLOCK$" || upper == L"CONIN$" || upper == L"CONOUT$") {
        return true;
    }
    return upper.size() == 4 && (upper.starts_with(L"COM") || upper.starts_with(L"LPT")) &&
           ((upper[3] >= L'1' && upper[3] <= L'9') || upper[3] == L'\u00b9' ||
            upper[3] == L'\u00b2' || upper[3] == L'\u00b3');
}

#ifdef _WIN32
struct CaseInsensitiveLess {
    bool operator()(const std::wstring &a, const std::wstring &b) const {
        return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(),
                                    static_cast<int>(b.size()), TRUE) == CSTR_LESS_THAN;
    }
};
#else
// Only ASCII paths reach the manifest check off Windows (see decode_path), and for ASCII the
// ordinal ignore-case rule of CompareStringOrdinal is exactly a-z -> A-Z.
struct CaseInsensitiveLess {
    static wchar_t upper(wchar_t c) {
        return c >= L'a' && c <= L'z' ? static_cast<wchar_t>(c - L'a' + L'A') : c;
    }
    bool operator()(const std::wstring &a, const std::wstring &b) const {
        return std::ranges::lexicographical_compare(
            a, b, [](wchar_t x, wchar_t y) { return upper(x) < upper(y); });
    }
};

// Well-formed UTF-8 as in Unicode table 3-7 (what MB_ERR_INVALID_CHARS accepts): no overlong
// forms, surrogates, code points above U+10FFFF, stray or missing continuation bytes. Used so
// that malformed paths keep the same error (unsafe_path) as on Windows.
bool strict_utf8(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size()) {
        const auto lead = static_cast<unsigned char>(text[i]);
        if (lead < 0x80) {
            ++i;
            continue;
        }
        std::size_t length = 0;
        unsigned char low = 0x80;
        unsigned char high = 0xbf;
        if (lead >= 0xc2 && lead <= 0xdf) {
            length = 2;
        } else if (lead >= 0xe0 && lead <= 0xef) {
            length = 3;
            low = lead == 0xe0 ? 0xa0 : 0x80;
            high = lead == 0xed ? 0x9f : 0xbf;
        } else if (lead >= 0xf0 && lead <= 0xf4) {
            length = 4;
            low = lead == 0xf0 ? 0x90 : 0x80;
            high = lead == 0xf4 ? 0x8f : 0xbf;
        } else {
            return false;
        }
        if (text.size() - i < length) {
            return false;
        }
        for (std::size_t k = 1; k < length; ++k) {
            const auto byte = static_cast<unsigned char>(text[i + k]);
            const auto min = k == 1 ? low : static_cast<unsigned char>(0x80);
            const auto max = k == 1 ? high : static_cast<unsigned char>(0xbf);
            if (byte < min || byte > max) {
                return false;
            }
        }
        i += length;
    }
    return true;
}
#endif

// Strict UTF-8 -> UTF-16 code units, already NFC. Off Windows only the ASCII subset of the
// (Windows) archive path profile is implemented: NFC and ordinal case folding of non-ASCII text
// need the host's Unicode tables, so any non-ASCII byte is refused instead of guessed (L1).
std::expected<std::wstring, Error> decode_path(std::string_view path) {
#ifdef _WIN32
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(),
                                          static_cast<int>(path.size()), nullptr, 0);
    if (count == 0) {
        return std::unexpected(unsafe("archive path is not valid UTF-8"));
    }
    std::wstring wide(static_cast<std::size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(),
                            static_cast<int>(path.size()), wide.data(), count) != count) {
        return std::unexpected(unsafe("archive path UTF-8 conversion failed"));
    }
    SetLastError(ERROR_SUCCESS);
    if (!IsNormalizedString(NormalizationC, wide.c_str(), -1)) {
        return std::unexpected(unsafe("archive path must already be NFC normalized"));
    }
    return wide;
#else
    if (!strict_utf8(path)) {
        return std::unexpected(unsafe("archive path is not valid UTF-8"));
    }
    std::wstring wide;
    wide.reserve(path.size());
    for (const char c : path) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte >= 0x80) {
            return std::unexpected(Error{ErrorCode::unsupported_feature,
                                         "non-ASCII archive paths are not supported on this "
                                         "platform yet"});
        }
        wide.push_back(static_cast<wchar_t>(byte));
    }
    return wide;
#endif
}
} // namespace

std::expected<std::vector<std::wstring>, Error> validate_archive_path(std::string_view path,
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
    const std::wstring &wide = *decoded;
    for (const auto c : wide) {
        if (c < 32 || c == 127 ||
            std::wstring_view{L"\\:<>\"|?*~"}.find(c) != std::wstring_view::npos) {
            return std::unexpected(unsafe("archive path contains a prohibited character"));
        }
    }
    std::vector<std::wstring> parts;
    std::size_t begin = 0;
    while (begin <= wide.size()) {
        const auto slash = wide.find(L'/', begin);
        const auto end = slash == std::wstring::npos ? wide.size() : slash;
        const auto part = std::wstring_view{wide}.substr(begin, end - begin);
        if (part.empty() || part == L"." || part == L".." || part.back() == L'.' ||
            part.back() == L' ' || reserved_name(part)) {
            return std::unexpected(unsafe("archive path contains an unsafe component"));
        }
        if (part.size() > 255 || parts.size() >= limits.max_components || parts.size() >= 64) {
            return std::unexpected(
                Error{ErrorCode::resource_limit, "archive path component limit"});
        }
        parts.emplace_back(part);
        if (slash == std::wstring::npos) {
            break;
        }
        begin = slash + 1;
    }
    return parts;
}

std::expected<void, Error> validate_manifest_paths(std::span<const PathEntry> entries,
                                                   const PathLimits &limits) {
    if (entries.size() > limits.max_entries || entries.size() > 100000) {
        return std::unexpected(Error{ErrorCode::resource_limit, "manifest entry limit"});
    }
    std::map<std::wstring, bool, CaseInsensitiveLess> names;
    std::size_t used = 0;
    const auto budget = (std::min)(limits.max_metadata_bytes, std::size_t{16 * 1024 * 1024});
    for (const auto &entry : entries) {
        if (used > budget || budget - used < 32 || entry.path.size() > budget - used - 32) {
            return std::unexpected(Error{ErrorCode::resource_limit, "manifest metadata limit"});
        }
        used += 32 + entry.path.size();
        const auto parsed = validate_archive_path(entry.path, limits);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        std::wstring name;
        for (const auto &component : *parsed) {
            if (!name.empty()) {
                name += L'/';
            }
            name += component;
        }
        if (!names.emplace(std::move(name), entry.directory).second) {
            return std::unexpected(unsafe("duplicate or case-colliding archive path"));
        }
    }
    for (const auto &item : names) {
        const auto &name = item.first;
        for (auto slash = name.find(L'/'); slash != std::wstring::npos;
             slash = name.find(L'/', slash + 1)) {
            const auto ancestor = names.find(name.substr(0, slash));
            if (ancestor != names.end() && !ancestor->second) {
                return std::unexpected(unsafe("file is also used as an archive directory"));
            }
        }
    }
    return {};
}
} // namespace seedarc::experimental
