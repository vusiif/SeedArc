// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Research-only Zstandard dictionary adapter (DEV-013, B-level internal interface). It lives
// inside the codec boundary so no zstd header escapes it, and it is built as a separate research
// target: the production library, the archive format and its decoder do not use it. Nothing here
// defines or freezes a dictionary format; a dictionary is an opaque byte string to callers.

#include "seedarc/error.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <vector>

namespace seedarc::experimental::codec::research {
// Resource ceilings. Decoding memory is bounded by the dictionary plus the output buffer.
inline constexpr std::size_t max_dictionary_bytes = 1024 * 1024;
inline constexpr std::size_t min_dictionary_bytes = 256;
inline constexpr std::size_t max_training_bytes = 64 * 1024 * 1024;
inline constexpr std::size_t max_training_samples = 1 << 20;

// Trains a dictionary of at most `capacity` bytes from `samples` (concatenated) split by
// `sizes`. Fails cleanly (invalid_argument / resource_limit / unsupported_feature) instead of
// returning a degenerate dictionary; callers then compress without one.
[[nodiscard]] std::expected<std::vector<std::byte>, Error>
train_dictionary(std::span<const std::byte> samples, std::span<const std::size_t> sizes,
                 std::size_t capacity);

// The dictionary ID a frame or dictionary declares (0: none / raw content).
[[nodiscard]] std::uint32_t dictionary_id(std::span<const std::byte> dictionary) noexcept;

// Same contract as codec::Encoder: whole blocks, explicit parameters, single thread; returns 0
// when the encoded form does not fit `output`, and callers pass output.size() < input.size().
// Frames carry content size and the dictionary ID, no checksum.
class DictionaryEncoder {
  public:
    [[nodiscard]] static std::expected<DictionaryEncoder, Error>
    create(std::uint32_t level, std::span<const std::byte> dictionary);
    DictionaryEncoder(DictionaryEncoder &&) noexcept;
    DictionaryEncoder &operator=(DictionaryEncoder &&) noexcept;
    ~DictionaryEncoder();

    [[nodiscard]] std::expected<std::size_t, Error> encode(std::span<const std::byte> input,
                                                           std::span<std::byte> output);

  private:
    struct Impl;
    explicit DictionaryEncoder(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// Decodes exactly one frame made with the same dictionary into exactly output.size() bytes.
// Rejects a missing / different dictionary ID, trailing input, a wrong content size and a
// declared window larger than the dictionary plus the block needs.
class DictionaryDecoder {
  public:
    [[nodiscard]] static std::expected<DictionaryDecoder, Error>
    create(std::span<const std::byte> dictionary);
    DictionaryDecoder(DictionaryDecoder &&) noexcept;
    DictionaryDecoder &operator=(DictionaryDecoder &&) noexcept;
    ~DictionaryDecoder();

    [[nodiscard]] std::expected<void, Error> decode(std::span<const std::byte> input,
                                                    std::span<std::byte> output);

  private:
    struct Impl;
    explicit DictionaryDecoder(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};
} // namespace seedarc::experimental::codec::research
