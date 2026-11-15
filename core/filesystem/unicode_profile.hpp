// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Internal to libseedarc and its tests: the Unicode operations of the archive path profile
// (ADR-0005), computed from tables generated from one pinned Unicode Character Database
// version, so that every platform and OS version gives the same answer.

#include <optional>
#include <string>
#include <string_view>

namespace seedarc::experimental::internal::unicode {
// The pinned UCD version, e.g. "17.0.0".
[[nodiscard]] std::string_view ucd_version() noexcept;

// Strict UTF-8 (Unicode table 3-7): no overlong forms, surrogates, code points above U+10FFFF,
// stray or missing continuation bytes. nullopt when malformed.
[[nodiscard]] std::optional<std::u32string> decode_utf8(std::string_view text);

[[nodiscard]] std::string encode_utf8(std::u32string_view text);

// Assigned in the pinned version (general category is not Cn; noncharacters are Cn).
[[nodiscard]] bool is_assigned(char32_t code_point) noexcept;

// Canonical combining class (0 for starters and unassigned code points).
[[nodiscard]] unsigned combining_class(char32_t code_point) noexcept;

// Normalization Form C (UAX #15) of a sequence of scalar values.
[[nodiscard]] std::u32string to_nfc(std::u32string_view text);

[[nodiscard]] bool is_nfc(std::u32string_view text);

// Simple case folding: CaseFolding.txt status C and S; identity when there is no mapping.
[[nodiscard]] char32_t simple_fold(char32_t code_point) noexcept;
} // namespace seedarc::experimental::internal::unicode
