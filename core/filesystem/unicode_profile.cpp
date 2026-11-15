// SPDX-License-Identifier: GPL-3.0-or-later
#include "filesystem/unicode_profile.hpp"

#include <algorithm>
#include <cstdint>
#include <span>

namespace seedarc::experimental::internal::unicode {
namespace {
namespace tables {
struct CodeRange {
    char32_t first;
    char32_t last;
};
struct CombiningClass {
    char32_t code_point;
    std::uint8_t combining_class;
};
struct Decomposition {
    char32_t code_point;
    std::uint16_t offset; // into decomposition_pool
    std::uint8_t length;
};
struct Composition {
    char32_t first;
    char32_t second;
    char32_t composite;
};
struct SimpleFold {
    char32_t code_point;
    char32_t folded;
};
#include "filesystem/unicode_tables.inc"
} // namespace tables

// Hangul syllable algorithm (Unicode section 3.12).
constexpr char32_t s_base = 0xAC00;
constexpr char32_t l_base = 0x1100;
constexpr char32_t v_base = 0x1161;
constexpr char32_t t_base = 0x11A7;
constexpr char32_t l_count = 19;
constexpr char32_t v_count = 21;
constexpr char32_t t_count = 28;
constexpr char32_t n_count = v_count * t_count;
constexpr char32_t s_count = l_count * n_count;

// Binary search of a table sorted by `field` (the generator emits every table sorted).
template <typename Range, typename Key, typename Row>
const Row *find_row(const Range &rows, Key key, Key Row::*field) noexcept {
    const auto it = std::ranges::lower_bound(rows, key, {}, field);
    return it != std::ranges::end(rows) && (*it).*field == key ? &*it : nullptr;
}

void decompose_into(char32_t c, std::u32string &out) {
    if (c >= s_base && c < s_base + s_count) {
        const auto index = c - s_base;
        out.push_back(l_base + index / n_count);
        out.push_back(v_base + (index % n_count) / t_count);
        if (const auto t = index % t_count; t != 0) {
            out.push_back(t_base + t);
        }
        return;
    }
    if (const auto *row = find_row(tables::decompositions, c, &tables::Decomposition::code_point)) {
        const auto pool = std::span{tables::decomposition_pool}.subspan(row->offset, row->length);
        out.append(pool.begin(), pool.end());
        return;
    }
    out.push_back(c);
}

// Primary composite of a canonically adjacent pair, or nullopt.
std::optional<char32_t> compose_pair(char32_t first, char32_t second) noexcept {
    if (first >= l_base && first < l_base + l_count && second >= v_base &&
        second < v_base + v_count) {
        return s_base + ((first - l_base) * v_count + (second - v_base)) * t_count;
    }
    if (first >= s_base && first < s_base + s_count && (first - s_base) % t_count == 0 &&
        second > t_base && second < t_base + t_count) {
        return first + (second - t_base);
    }
    const auto it = std::ranges::lower_bound(
        tables::compositions, std::pair{first, second}, {},
        [](const tables::Composition &row) { return std::pair{row.first, row.second}; });
    if (it != std::end(tables::compositions) && it->first == first && it->second == second) {
        return it->composite;
    }
    return std::nullopt;
}
} // namespace

std::string_view ucd_version() noexcept { return tables::ucd_version; }

std::optional<std::u32string> decode_utf8(std::string_view text) {
    std::u32string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        const auto lead = static_cast<unsigned char>(text[i]);
        if (lead < 0x80) {
            out.push_back(lead);
            ++i;
            continue;
        }
        std::size_t length = 0;
        unsigned char low = 0x80;
        unsigned char high = 0xbf;
        char32_t value = 0;
        if (lead >= 0xc2 && lead <= 0xdf) {
            length = 2;
            value = lead & 0x1fU;
        } else if (lead >= 0xe0 && lead <= 0xef) {
            length = 3;
            low = lead == 0xe0 ? 0xa0 : 0x80;
            high = lead == 0xed ? 0x9f : 0xbf;
            value = lead & 0x0fU;
        } else if (lead >= 0xf0 && lead <= 0xf4) {
            length = 4;
            low = lead == 0xf0 ? 0x90 : 0x80;
            high = lead == 0xf4 ? 0x8f : 0xbf;
            value = lead & 0x07U;
        } else {
            return std::nullopt;
        }
        if (text.size() - i < length) {
            return std::nullopt;
        }
        for (std::size_t k = 1; k < length; ++k) {
            const auto byte = static_cast<unsigned char>(text[i + k]);
            const auto min = k == 1 ? low : static_cast<unsigned char>(0x80);
            const auto max = k == 1 ? high : static_cast<unsigned char>(0xbf);
            if (byte < min || byte > max) {
                return std::nullopt;
            }
            value = (value << 6) | (byte & 0x3fU);
        }
        out.push_back(value);
        i += length;
    }
    return out;
}

std::string encode_utf8(std::u32string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const auto c : text) {
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
        } else if (c < 0x800) {
            out.push_back(static_cast<char>(0xc0 | (c >> 6)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3f)));
        } else if (c < 0x10000) {
            out.push_back(static_cast<char>(0xe0 | (c >> 12)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3f)));
        } else {
            out.push_back(static_cast<char>(0xf0 | (c >> 18)));
            out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3f)));
        }
    }
    return out;
}

bool is_assigned(char32_t code_point) noexcept {
    const auto it = std::ranges::upper_bound(tables::assigned_ranges, code_point, {},
                                             &tables::CodeRange::first);
    return it != std::begin(tables::assigned_ranges) && code_point <= std::prev(it)->last;
}

unsigned combining_class(char32_t code_point) noexcept {
    const auto *row =
        find_row(tables::combining_classes, code_point, &tables::CombiningClass::code_point);
    return row == nullptr ? 0U : row->combining_class;
}

std::u32string to_nfc(std::u32string_view text) {
    // 1. Full canonical decomposition.
    std::u32string d;
    d.reserve(text.size() * 2);
    for (const auto c : text) {
        decompose_into(c, d);
    }
    // 2. Canonical ordering: stable sort of every run of non-starters by combining class.
    for (std::size_t i = 0; i < d.size();) {
        if (combining_class(d[i]) == 0) {
            ++i;
            continue;
        }
        auto end = i;
        while (end < d.size() && combining_class(d[end]) != 0) {
            ++end;
        }
        std::stable_sort(d.begin() + static_cast<std::ptrdiff_t>(i),
                         d.begin() + static_cast<std::ptrdiff_t>(end), [](char32_t a, char32_t b) {
                             return combining_class(a) < combining_class(b);
                         });
        i = end;
    }
    // 3. Canonical composition (UAX #15 section 10 reference algorithm).
    if (d.empty()) {
        return d;
    }
    std::size_t starter = 0;
    unsigned last_class = combining_class(d[0]);
    if (last_class != 0) {
        last_class = 256; // a leading non-starter blocks every later composition with it
    }
    std::size_t write = 1;
    for (std::size_t read = 1; read < d.size(); ++read) {
        const auto c = d[read];
        const auto c_class = combining_class(c);
        const auto composite = compose_pair(d[starter], c);
        if (composite && (last_class < c_class || last_class == 0)) {
            d[starter] = *composite;
            continue;
        }
        if (c_class == 0) {
            starter = write;
        }
        last_class = c_class;
        d[write++] = c;
    }
    d.resize(write);
    return d;
}

bool is_nfc(std::u32string_view text) {
    if (std::ranges::all_of(text, [](char32_t c) { return c < 0x80; })) {
        return true; // ASCII has no decompositions, compositions or combining marks
    }
    return to_nfc(text) == text;
}

char32_t simple_fold(char32_t code_point) noexcept {
    const auto *row = find_row(tables::simple_folds, code_point, &tables::SimpleFold::code_point);
    return row == nullptr ? code_point : row->folded;
}
} // namespace seedarc::experimental::internal::unicode
