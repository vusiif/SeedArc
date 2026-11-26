// SPDX-License-Identifier: GPL-3.0-or-later
// Fuzz target for the research container (append4_study, revisions 4 and 5): the strict parser
// (index + record scan cross-check, recipe label check), full decoding of an accepted container,
// and prefix / damaged-file recovery. Each input runs twice: as given, and with every checksum
// recomputed so that mutations reach the field validation behind the CRCs.
//
// Accepted containers must decode or fail cleanly; nothing may read out of bounds, overflow,
// leak or allocate without a bound (ASan / UBSan / libFuzzer limits enforce the rest).
#define SEEDARC_APPEND4_NO_MAIN
#ifndef SEEDARC_APPEND4_MAX_FRAME_RAW
#define SEEDARC_APPEND4_MAX_FRAME_RAW (std::uint64_t{64} << 20)
#endif
#include "append4_study.cpp"

namespace {
void require(bool condition) {
    if (!condition) {
        std::abort();
    }
}

std::uint32_t le32(std::span<const std::byte> b, std::size_t at) {
    std::uint32_t v = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        v |= std::to_integer<std::uint32_t>(b[at + i]) << (8 * i);
    }
    return v;
}
std::uint64_t le64(std::span<const std::byte> b, std::size_t at) {
    return le32(b, at) | (std::uint64_t{le32(b, at + 4)} << 32);
}
void store32(std::span<std::byte> b, std::size_t at, std::uint32_t v) {
    for (std::size_t i = 0; i < 4; ++i) {
        b[at + i] = static_cast<std::byte>((v >> (8 * i)) & 0xffU);
    }
}

// Recompute the checksums a writer would have produced: each record's body CRC, the header CRC
// at the start of the trailer and the trailer CRC in the footer. Offsets and lengths are left as
// the mutation made them.
void repair(std::span<std::byte> b) {
    for (std::size_t at = 0; at + record_overhead <= b.size(); ++at) {
        bool tagged = false;
        for (const auto &tag : record_tags) {
            tagged = tagged || std::memcmp(b.data() + at, tag.data(), tag.size()) == 0;
        }
        if (!tagged) {
            continue;
        }
        const auto len = le32(b, at + 8);
        if (len > max_record_body || b.size() - at - record_overhead < len) {
            continue;
        }
        const auto body = std::span<const std::byte>{b}.subspan(at + 12, len);
        store32(b, at + 12 + len, seedarc::experimental::crc32c(body));
    }
    if (b.size() < header_bytes + footer_bytes) {
        return;
    }
    const auto footer = b.size() - footer_bytes;
    const auto offset = le64(b, footer), size = std::uint64_t{le32(b, footer + 8)};
    if (offset < header_bytes || offset > footer || footer - offset != size) {
        return;
    }
    const auto trailer = b.subspan(static_cast<std::size_t>(offset), static_cast<std::size_t>(size));
    if (trailer.size() >= 4) {
        store32(trailer, 0, seedarc::experimental::crc32c(std::span<const std::byte>{b}.first(header_bytes)));
    }
    store32(b, footer + 12, seedarc::experimental::crc32c(trailer));
}

void exercise(std::span<const std::byte> bytes) {
    if (auto m = parse(bytes)) {
        require(!m->gens.empty() && m->trailer_offset >= header_bytes &&
                m->trailer_offset < bytes.size());
        for (const auto &e : m->entries) {
            require(e.gen < m->gens.size());
            require(!m->header.solid() || e.frame < m->frames.size());
        }
        for (const auto &f : m->frames) {
            require(f.raw != 0 && f.raw <= max_frame_raw && f.stored != 0 &&
                    f.offset <= bytes.size() && f.stored <= bytes.size() - f.offset);
        }
        // Decoding an accepted container either succeeds (every file CRC matched) or fails
        // with an error; both are fine, a crash or wrong-size view is not.
        (void)visit_decoded(*m, bytes,
                            [&](const Entry &e, std::size_t at,
                                std::span<const std::byte> raw) -> std::expected<void, Error> {
                                require(at <= e.size && raw.size() <= e.size - at);
                                return {};
                            });
    }
    if (auto r = recover(bytes)) {
        for (const auto &[path, data] : r->files) {
            require(!path.empty());
            (void)data;
        }
    }
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    const auto bytes = std::as_bytes(std::span{data, size});
    exercise(bytes);
    std::vector<std::byte> fixed(bytes.begin(), bytes.end());
    repair(fixed);
    if (!std::ranges::equal(fixed, bytes)) {
        exercise(fixed);
    }
    return 0;
}
