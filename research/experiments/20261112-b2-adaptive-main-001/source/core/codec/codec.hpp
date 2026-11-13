// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Internal codec boundary (ADR-0003). The only place that touches the zstd and liblzma C APIs;
// no third-party header or error code escapes it. Not installed and not part of the public API.

#include "seedarc/error.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>

namespace seedarc::experimental::codec {
// Block codec identifiers as stored in the index (BlockDescriptor::codec_id).
inline constexpr std::uint32_t store_id = 0;
inline constexpr std::uint32_t zstd_id = 1;
inline constexpr std::uint32_t lzma2_id = 2;

// codec_config_id ranges: Store uses 0; Zstandard the compression level; LZMA2 the liblzma preset.
// The configuration only steers the encoder; decoding never depends on it.
inline constexpr std::uint32_t zstd_min_level = 1;
inline constexpr std::uint32_t zstd_max_level = 19;
inline constexpr std::uint32_t lzma2_max_preset = 9;

[[nodiscard]] bool valid_block_codec(std::uint32_t id, std::uint32_t config) noexcept;

// Encodes whole blocks with fixed, explicit parameters and a single thread.
class Encoder {
  public:
    [[nodiscard]] static std::expected<Encoder, Error> create(std::uint32_t id,
                                                              std::uint32_t config);
    Encoder(Encoder &&) noexcept;
    Encoder &operator=(Encoder &&) noexcept;
    ~Encoder();

    // Writes one complete encoded stream of `input` into `output` and returns its length, or 0
    // when the encoded form does not fit `output` (the caller then stores the block raw). Callers
    // pass `output.size() < input.size()`, so a non-zero result is always strictly smaller.
    [[nodiscard]] std::expected<std::size_t, Error> encode(std::span<const std::byte> input,
                                                           std::span<std::byte> output);

  private:
    struct Impl;
    explicit Encoder(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// Decodes blocks. `input` must be exactly one complete stream that decodes to exactly
// `output.size()` bytes; trailing input, short output and oversized windows are rejected. Memory
// is bounded by the output size.
class Decoder {
  public:
    Decoder();
    Decoder(Decoder &&) noexcept;
    Decoder &operator=(Decoder &&) noexcept;
    ~Decoder();

    [[nodiscard]] std::expected<void, Error>
    decode(std::uint32_t id, std::span<const std::byte> input, std::span<std::byte> output);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// "zstd 1.5.7, liblzma 5.8.4": the linked library versions, for reports and diagnostics.
[[nodiscard]] std::string library_versions();
} // namespace seedarc::experimental::codec
