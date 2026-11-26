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

// DEV-017 solid baseline: one zstd frame over a whole input (no dictionary), window
// min(27, ceil(log2(size))) and content size recorded; decoding refuses larger windows,
// several frames, trailing bytes and a content size that differs from output.size().
inline constexpr unsigned max_solid_window_log = 27;
inline constexpr std::size_t max_solid_bytes = std::size_t{1} << 30;
[[nodiscard]] std::expected<std::vector<std::byte>, Error>
compress_solid(std::span<const std::byte> input, std::uint32_t level);
[[nodiscard]] std::expected<void, Error> decompress_solid(std::span<const std::byte> frame,
                                                          std::span<std::byte> output);
// DEV-017 partial recovery: streams a possibly truncated solid frame (`prefix` is its first
// bytes) into `output` and returns how many leading bytes were produced. Same window limit and
// no dictionary; never writes past output.size(). The produced bytes carry no checksum of their
// own: callers verify every file they cut out of them.
[[nodiscard]] std::expected<std::size_t, Error>
decompress_solid_prefix(std::span<const std::byte> prefix, std::span<std::byte> output);

// append-drift probe (research/ideas/append-drift-r0.md): one zstd frame over `input` that may
// refer to `reference`, either a trained dictionary (raw_prefix false) or raw content that comes
// before the input (raw_prefix true; decoding then needs exactly those bytes). Window
// min(27, ceil(log2(reference + input))), content size recorded, no checksum. Not used by any
// archive format; measurement only.
// `long_distance` forces zstd long-distance matching on (encoder only; decoding is unchanged);
// false leaves zstd's default, which enables it by itself for level 16+ with a 2^27 window.
[[nodiscard]] std::expected<std::vector<std::byte>, Error>
compress_solid_referenced(std::span<const std::byte> input, std::uint32_t level,
                          std::span<const std::byte> reference, bool raw_prefix,
                          bool long_distance = false);
[[nodiscard]] std::expected<void, Error>
decompress_solid_referenced(std::span<const std::byte> frame, std::span<const std::byte> reference,
                            bool raw_prefix, std::span<std::byte> output);

// Solid frames by recipe codec (ADR-0006): solid_zstd = the functions above (levels 1-19);
// solid_lzma2 = one raw LZMA2 stream (liblzma preset 0-9, dictionary capped at the input size,
// end marker required). Decoding needs no recorded parameters: the LZMA2 dictionary is sized
// to the output, which bounds every match distance, and memory is checked before allocation.
inline constexpr std::uint32_t solid_zstd = 0;
inline constexpr std::uint32_t solid_lzma2 = 1;
[[nodiscard]] bool valid_solid_codec(std::uint32_t codec, std::uint32_t level) noexcept;
// `extreme` (LZMA2 only, ADR-0007) adds LZMA_PRESET_EXTREME: an encoder-only search option, the
// frame is the same raw LZMA2 stream and decodes with decompress_solid_codec unchanged.
[[nodiscard]] std::expected<std::vector<std::byte>, Error>
compress_solid_codec(std::uint32_t codec, std::uint32_t level, std::span<const std::byte> input,
                     bool extreme = false);
[[nodiscard]] std::expected<void, Error>
decompress_solid_codec(std::uint32_t codec, std::span<const std::byte> frame,
                       std::span<std::byte> output);
// As decompress_solid_prefix: stops when `output` is full, the stream ends or no progress is
// possible; returns the leading bytes produced, which callers must verify themselves.
[[nodiscard]] std::expected<std::size_t, Error>
decompress_solid_prefix_codec(std::uint32_t codec, std::span<const std::byte> prefix,
                              std::span<std::byte> output);

// codec-capability probe (research/ideas/codec-capabilities-r0.md): solid frames with encoder
// options the recipes do not use yet. Measurement only; no archive format records them.
//   zstd:  level 1-22, `long_distance` forces LDM on (false leaves zstd's own default, which
//          turns LDM on for level 16+ once the window reaches 2^27), `workers` = nbWorkers
//          (0 = single-threaded API);
//          window min(27, ceil(log2(size))) as for every solid frame, content size recorded.
//   lzma2: preset 0-9, `extreme` (LZMA_PRESET_EXTREME), `bcj_x86` (x86 BCJ before LZMA2).
// Decoding needs the same `codec` and `bcj_x86`; the other options change only the encoder.
struct SolidVariant {
    std::uint32_t codec = solid_zstd;
    std::uint32_t level = 9;
    bool long_distance = false;
    std::uint32_t workers = 0;
    bool extreme = false;
    bool bcj_x86 = false;
};
[[nodiscard]] bool valid_solid_variant(const SolidVariant &variant) noexcept;
[[nodiscard]] std::expected<std::vector<std::byte>, Error>
compress_solid_variant(const SolidVariant &variant, std::span<const std::byte> input);
[[nodiscard]] std::expected<void, Error>
decompress_solid_variant(const SolidVariant &variant, std::span<const std::byte> frame,
                         std::span<std::byte> output);

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
