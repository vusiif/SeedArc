// SPDX-License-Identifier: GPL-3.0-or-later
#include "codec/codec.hpp"

#include <lzma.h>
#include <zstd.h>
#include <zstd_errors.h>

#include <algorithm>
#include <array>
#include <string>
#include <utility>

namespace seedarc::experimental::codec {
namespace {
Error corrupt(std::string what) {
    return {ErrorCode::integrity_error, "corrupt compressed block: " + std::move(what)};
}

Error unknown_codec() {
    return {ErrorCode::unsupported_feature, "unsupported codec or codec configuration"};
}

struct ZstdCompressionContextDeleter {
    void operator()(ZSTD_CCtx *context) const noexcept { ZSTD_freeCCtx(context); }
};
struct ZstdDecompressionContextDeleter {
    void operator()(ZSTD_DCtx *context) const noexcept { ZSTD_freeDCtx(context); }
};

// Smallest window log that covers `size` bytes, within the range of the Zstandard frame format
// (RFC 8878: 10 to 31). The named constants are only in zstd's experimental API.
int window_log_for(std::size_t size) noexcept {
    constexpr int min_window_log = 10;
    constexpr int max_window_log = 31;
    int log = min_window_log;
    while (log < max_window_log && (std::size_t{1} << log) < size) {
        ++log;
    }
    return log;
}

bool zstd_ok(std::size_t result) noexcept { return ZSTD_isError(result) == 0; }

// Window size declared by a regular frame (RFC 8878 section 3.1.1.1); a single-segment frame
// uses its content size. Called after the frame and its content size were validated.
std::uint64_t zstd_window_size(std::span<const std::byte> frame, std::uint64_t content) noexcept {
    const auto descriptor = std::to_integer<unsigned>(frame[4]);
    if ((descriptor & 0x20U) != 0) {
        return content;
    }
    const auto window = std::to_integer<unsigned>(frame[5]);
    const auto base = std::uint64_t{1} << (10 + (window >> 3));
    return base + (base / 8) * (window & 7U);
}

// The LZMA2 decoder only needs a dictionary that covers every match distance; no distance can
// exceed the decoded size, so the block size is always enough and the encoder's choice of
// dictionary never has to be recorded.
std::uint32_t lzma2_dictionary_for(std::size_t size) noexcept {
    return static_cast<std::uint32_t>(
        std::clamp<std::size_t>(size, LZMA_DICT_SIZE_MIN, std::uint32_t{1} << 30));
}
} // namespace

bool valid_block_codec(std::uint32_t id, std::uint32_t config) noexcept {
    switch (id) {
    case store_id:
        return config == 0;
    case zstd_id:
        return config >= zstd_min_level && config <= zstd_max_level;
    case lzma2_id:
        return config <= lzma2_max_preset;
    default:
        return false;
    }
}

struct Encoder::Impl {
    std::uint32_t id = store_id;
    std::uint32_t config = 0;
    std::unique_ptr<ZSTD_CCtx, ZstdCompressionContextDeleter> zstd;
};

Encoder::Encoder(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Encoder::Encoder(Encoder &&) noexcept = default;
Encoder &Encoder::operator=(Encoder &&) noexcept = default;
Encoder::~Encoder() = default;

std::expected<Encoder, Error> Encoder::create(std::uint32_t id, std::uint32_t config) {
    if (id == store_id || !valid_block_codec(id, config)) {
        return std::unexpected(
            Error{ErrorCode::invalid_argument, "invalid codec or compression level"});
    }
    auto impl = std::make_unique<Impl>();
    impl->id = id;
    impl->config = config;
    if (id == zstd_id) {
        impl->zstd.reset(ZSTD_createCCtx());
        if (!impl->zstd) {
            return std::unexpected(
                Error{ErrorCode::resource_limit, "cannot allocate zstd compression context"});
        }
    }
    return Encoder{std::move(impl)};
}

std::expected<std::size_t, Error> Encoder::encode(std::span<const std::byte> input,
                                                  std::span<std::byte> output) {
    if (input.empty() || output.size() >= input.size()) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "invalid encode buffers"});
    }
    if (output.empty()) {
        return 0;
    }
    if (impl_->id == zstd_id) {
        auto *context = impl_->zstd.get();
        // Every parameter that affects the output is set explicitly for each block.
        if (!zstd_ok(ZSTD_CCtx_reset(context, ZSTD_reset_session_and_parameters)) ||
            !zstd_ok(ZSTD_CCtx_setParameter(context, ZSTD_c_compressionLevel,
                                            static_cast<int>(impl_->config))) ||
            !zstd_ok(ZSTD_CCtx_setParameter(context, ZSTD_c_checksumFlag, 0)) ||
            !zstd_ok(ZSTD_CCtx_setParameter(context, ZSTD_c_contentSizeFlag, 1)) ||
            !zstd_ok(ZSTD_CCtx_setParameter(context, ZSTD_c_dictIDFlag, 0)) ||
            !zstd_ok(ZSTD_CCtx_setParameter(context, ZSTD_c_nbWorkers, 0))) {
            return std::unexpected(Error{ErrorCode::io_error, "cannot configure zstd encoder"});
        }
        const auto result =
            ZSTD_compress2(context, output.data(), output.size(), input.data(), input.size());
        if (zstd_ok(result)) {
            return result;
        }
        if (ZSTD_getErrorCode(result) == ZSTD_error_dstSize_tooSmall) {
            return 0;
        }
        return std::unexpected(Error{ErrorCode::io_error, std::string{"zstd compression failed: "} +
                                                              ZSTD_getErrorName(result)});
    }
    lzma_options_lzma options{};
    if (lzma_lzma_preset(&options, impl_->config) != 0) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "invalid LZMA2 preset"});
    }
    options.dict_size = (std::min)(options.dict_size, lzma2_dictionary_for(input.size()));
    const std::array<lzma_filter, 2> filters{
        {{LZMA_FILTER_LZMA2, &options}, {LZMA_VLI_UNKNOWN, nullptr}}};
    std::size_t written = 0;
    const auto result = lzma_raw_buffer_encode(
        filters.data(), nullptr, reinterpret_cast<const std::uint8_t *>(input.data()), input.size(),
        reinterpret_cast<std::uint8_t *>(output.data()), &written, output.size());
    if (result == LZMA_OK) {
        return written;
    }
    if (result == LZMA_BUF_ERROR) {
        return 0;
    }
    if (result == LZMA_MEM_ERROR || result == LZMA_MEMLIMIT_ERROR) {
        return std::unexpected(Error{ErrorCode::resource_limit, "LZMA2 encoder out of memory"});
    }
    return std::unexpected(
        Error{ErrorCode::io_error,
              "LZMA2 compression failed: code " + std::to_string(static_cast<int>(result))});
}

struct Decoder::Impl {
    std::unique_ptr<ZSTD_DCtx, ZstdDecompressionContextDeleter> zstd;
};

Decoder::Decoder() : impl_(std::make_unique<Impl>()) {}
Decoder::Decoder(Decoder &&) noexcept = default;
Decoder &Decoder::operator=(Decoder &&) noexcept = default;
Decoder::~Decoder() = default;

std::expected<void, Error> Decoder::decode(std::uint32_t id, std::span<const std::byte> input,
                                           std::span<std::byte> output) {
    if (input.empty() || output.empty()) {
        return std::unexpected(corrupt("empty block"));
    }
    if (id == zstd_id) {
        // Exactly one regular frame that declares exactly the expected content size.
        const auto frame = ZSTD_findFrameCompressedSize(input.data(), input.size());
        if (!zstd_ok(frame) || frame != input.size()) {
            return std::unexpected(corrupt("not exactly one zstd frame"));
        }
        const auto content = ZSTD_getFrameContentSize(input.data(), input.size());
        if (content == ZSTD_CONTENTSIZE_UNKNOWN || content == ZSTD_CONTENTSIZE_ERROR ||
            content != output.size()) {
            return std::unexpected(corrupt("zstd frame content size mismatch"));
        }
        if (!impl_->zstd) {
            impl_->zstd.reset(ZSTD_createDCtx());
            if (!impl_->zstd) {
                return std::unexpected(
                    Error{ErrorCode::resource_limit, "cannot allocate zstd decompression context"});
            }
        }
        // Single-shot decoding uses `output` as its history, so memory never depends on the
        // declared window; a window beyond what the block needs is still refused, as this
        // library's encoder never writes one.
        if (zstd_window_size(input, content) >
            (std::uint64_t{1} << window_log_for(output.size()))) {
            return std::unexpected(
                Error{ErrorCode::resource_limit, "zstd window is larger than the block"});
        }
        auto *context = impl_->zstd.get();
        if (!zstd_ok(ZSTD_DCtx_reset(context, ZSTD_reset_session_and_parameters))) {
            return std::unexpected(Error{ErrorCode::io_error, "cannot reset zstd decoder"});
        }
        const auto result =
            ZSTD_decompressDCtx(context, output.data(), output.size(), input.data(), input.size());
        if (!zstd_ok(result)) {
            if (ZSTD_getErrorCode(result) == ZSTD_error_frameParameter_windowTooLarge ||
                ZSTD_getErrorCode(result) == ZSTD_error_memory_allocation) {
                return std::unexpected(
                    Error{ErrorCode::resource_limit, "zstd block needs too much memory"});
            }
            return std::unexpected(corrupt(ZSTD_getErrorName(result)));
        }
        if (result != output.size()) {
            return std::unexpected(corrupt("zstd output size mismatch"));
        }
        return {};
    }
    if (id == lzma2_id) {
        lzma_options_lzma options{};
        options.dict_size = lzma2_dictionary_for(output.size());
        const std::array<lzma_filter, 2> filters{
            {{LZMA_FILTER_LZMA2, &options}, {LZMA_VLI_UNKNOWN, nullptr}}};
        // About the dictionary plus a small constant; checked before any allocation.
        const auto memory = lzma_raw_decoder_memusage(filters.data());
        if (memory == UINT64_MAX || memory > std::uint64_t{options.dict_size} + (1U << 20)) {
            return std::unexpected(
                Error{ErrorCode::resource_limit, "LZMA2 block needs too much memory"});
        }
        std::size_t consumed = 0;
        std::size_t produced = 0;
        const auto result = lzma_raw_buffer_decode(
            filters.data(), nullptr, reinterpret_cast<const std::uint8_t *>(input.data()),
            &consumed, input.size(), reinterpret_cast<std::uint8_t *>(output.data()), &produced,
            output.size());
        if (result == LZMA_MEM_ERROR || result == LZMA_MEMLIMIT_ERROR) {
            return std::unexpected(
                Error{ErrorCode::resource_limit, "LZMA2 block needs too much memory"});
        }
        // LZMA_OK here means the end marker was reached (liblzma maps LZMA_STREAM_END to OK).
        if (result != LZMA_OK || consumed != input.size() || produced != output.size()) {
            return std::unexpected(corrupt("LZMA2 stream is invalid, truncated or has trailing "
                                           "bytes (code " +
                                           std::to_string(static_cast<int>(result)) + ")"));
        }
        return {};
    }
    return std::unexpected(unknown_codec());
}

std::string library_versions() {
    return std::string{"zstd "} + ZSTD_versionString() + ", liblzma " + lzma_version_string();
}
} // namespace seedarc::experimental::codec
