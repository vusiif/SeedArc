// SPDX-License-Identifier: GPL-3.0-or-later
#include "codec/dictionary.hpp"

#include "codec/codec.hpp"

#include <zdict.h>
#include <zstd.h>
#include <zstd_errors.h>

#include <numeric>
#include <string>
#include <utility>

namespace seedarc::experimental::codec::research {
namespace {
Error corrupt(std::string what) {
    return {ErrorCode::integrity_error, "corrupt dictionary-compressed block: " + std::move(what)};
}
bool zstd_ok(std::size_t result) noexcept { return ZSTD_isError(result) == 0; }

// Same rules as the production adapter in codec.cpp (kept local so that file is unchanged).
int window_log_for(std::uint64_t size) noexcept {
    int log = 10;
    while (log < 31 && (std::uint64_t{1} << log) < size) {
        ++log;
    }
    return log;
}
std::uint64_t zstd_window_size(std::span<const std::byte> frame, std::uint64_t content) noexcept {
    const auto descriptor = std::to_integer<unsigned>(frame[4]);
    if ((descriptor & 0x20U) != 0) {
        return content;
    }
    const auto window = std::to_integer<unsigned>(frame[5]);
    const auto base = std::uint64_t{1} << (10 + (window >> 3));
    return base + (base / 8) * (window & 7U);
}

struct CDictDeleter {
    void operator()(ZSTD_CDict *d) const noexcept { ZSTD_freeCDict(d); }
};
struct DDictDeleter {
    void operator()(ZSTD_DDict *d) const noexcept { ZSTD_freeDDict(d); }
};
struct CCtxDeleter {
    void operator()(ZSTD_CCtx *c) const noexcept { ZSTD_freeCCtx(c); }
};
struct DCtxDeleter {
    void operator()(ZSTD_DCtx *c) const noexcept { ZSTD_freeDCtx(c); }
};

std::expected<void, Error> check_dictionary(std::span<const std::byte> dictionary) {
    if (dictionary.size() < min_dictionary_bytes || dictionary.size() > max_dictionary_bytes) {
        return std::unexpected(Error{ErrorCode::resource_limit, "dictionary size out of range"});
    }
    if (dictionary_id(dictionary) == 0) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "not a zstd dictionary"});
    }
    return {};
}
} // namespace

std::uint32_t dictionary_id(std::span<const std::byte> dictionary) noexcept {
    return dictionary.empty() ? 0U
                              : static_cast<std::uint32_t>(
                                    ZDICT_getDictID(dictionary.data(), dictionary.size()));
}

std::expected<std::vector<std::byte>, Error>
train_dictionary(std::span<const std::byte> samples, std::span<const std::size_t> sizes,
                 std::size_t capacity) {
    if (capacity < min_dictionary_bytes || capacity > max_dictionary_bytes) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "dictionary capacity"});
    }
    if (samples.size() > max_training_bytes || sizes.size() > max_training_samples) {
        return std::unexpected(Error{ErrorCode::resource_limit, "training input too large"});
    }
    if (std::accumulate(sizes.begin(), sizes.end(), std::size_t{0}) != samples.size()) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "sample sizes do not add up"});
    }
    std::vector<std::byte> dictionary(capacity);
    const auto result =
        ZDICT_trainFromBuffer(dictionary.data(), dictionary.size(), samples.data(), sizes.data(),
                              static_cast<unsigned>(sizes.size()));
    if (ZDICT_isError(result) != 0) {
        return std::unexpected(Error{ErrorCode::unsupported_feature,
                                     std::string{"dictionary training failed: "} +
                                         ZDICT_getErrorName(result)});
    }
    dictionary.resize(result);
    if (auto checked = check_dictionary(dictionary); !checked) {
        return std::unexpected(checked.error());
    }
    return dictionary;
}

struct DictionaryEncoder::Impl {
    std::unique_ptr<ZSTD_CDict, CDictDeleter> dictionary;
    std::unique_ptr<ZSTD_CCtx, CCtxDeleter> context;
};

DictionaryEncoder::DictionaryEncoder(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
DictionaryEncoder::DictionaryEncoder(DictionaryEncoder &&) noexcept = default;
DictionaryEncoder &DictionaryEncoder::operator=(DictionaryEncoder &&) noexcept = default;
DictionaryEncoder::~DictionaryEncoder() = default;

std::expected<DictionaryEncoder, Error>
DictionaryEncoder::create(std::uint32_t level, std::span<const std::byte> dictionary) {
    if (!valid_block_codec(zstd_id, level)) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "invalid zstd level"});
    }
    if (auto checked = check_dictionary(dictionary); !checked) {
        return std::unexpected(checked.error());
    }
    auto impl = std::make_unique<Impl>();
    impl->dictionary.reset(
        ZSTD_createCDict(dictionary.data(), dictionary.size(), static_cast<int>(level)));
    impl->context.reset(ZSTD_createCCtx());
    if (!impl->dictionary || !impl->context) {
        return std::unexpected(
            Error{ErrorCode::resource_limit, "cannot allocate zstd dictionary encoder"});
    }
    return DictionaryEncoder{std::move(impl)};
}

std::expected<std::size_t, Error> DictionaryEncoder::encode(std::span<const std::byte> input,
                                                            std::span<std::byte> output) {
    if (input.empty() || output.size() >= input.size()) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "invalid encode buffers"});
    }
    if (output.empty()) {
        return 0;
    }
    auto *context = impl_->context.get();
    if (!zstd_ok(ZSTD_CCtx_reset(context, ZSTD_reset_session_and_parameters)) ||
        !zstd_ok(ZSTD_CCtx_setParameter(context, ZSTD_c_checksumFlag, 0)) ||
        !zstd_ok(ZSTD_CCtx_setParameter(context, ZSTD_c_contentSizeFlag, 1)) ||
        !zstd_ok(ZSTD_CCtx_setParameter(context, ZSTD_c_dictIDFlag, 1)) ||
        !zstd_ok(ZSTD_CCtx_setParameter(context, ZSTD_c_nbWorkers, 0)) ||
        !zstd_ok(ZSTD_CCtx_refCDict(context, impl_->dictionary.get()))) {
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

struct DictionaryDecoder::Impl {
    std::unique_ptr<ZSTD_DDict, DDictDeleter> dictionary;
    std::unique_ptr<ZSTD_DCtx, DCtxDeleter> context;
    std::uint32_t id = 0;
    std::size_t size = 0;
};

DictionaryDecoder::DictionaryDecoder(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
DictionaryDecoder::DictionaryDecoder(DictionaryDecoder &&) noexcept = default;
DictionaryDecoder &DictionaryDecoder::operator=(DictionaryDecoder &&) noexcept = default;
DictionaryDecoder::~DictionaryDecoder() = default;

std::expected<DictionaryDecoder, Error>
DictionaryDecoder::create(std::span<const std::byte> dictionary) {
    if (auto checked = check_dictionary(dictionary); !checked) {
        return std::unexpected(checked.error());
    }
    auto impl = std::make_unique<Impl>();
    impl->dictionary.reset(ZSTD_createDDict(dictionary.data(), dictionary.size()));
    impl->context.reset(ZSTD_createDCtx());
    if (!impl->dictionary || !impl->context) {
        return std::unexpected(
            Error{ErrorCode::resource_limit, "cannot allocate zstd dictionary decoder"});
    }
    impl->id = dictionary_id(dictionary);
    impl->size = dictionary.size();
    return DictionaryDecoder{std::move(impl)};
}

std::expected<void, Error> DictionaryDecoder::decode(std::span<const std::byte> input,
                                                     std::span<std::byte> output) {
    if (input.empty() || output.empty()) {
        return std::unexpected(corrupt("empty block"));
    }
    const auto frame = ZSTD_findFrameCompressedSize(input.data(), input.size());
    if (!zstd_ok(frame) || frame != input.size()) {
        return std::unexpected(corrupt("not exactly one zstd frame"));
    }
    const auto content = ZSTD_getFrameContentSize(input.data(), input.size());
    if (content == ZSTD_CONTENTSIZE_UNKNOWN || content == ZSTD_CONTENTSIZE_ERROR ||
        content != output.size()) {
        return std::unexpected(corrupt("zstd frame content size mismatch"));
    }
    if (ZSTD_getDictID_fromFrame(input.data(), input.size()) != impl_->id) {
        return std::unexpected(corrupt("frame does not use this dictionary"));
    }
    // Single-shot decoding keeps history in `output` plus the dictionary, so a declared window
    // beyond dictionary + block is refused, as the encoder never needs one.
    if (zstd_window_size(input, content) >
        (std::uint64_t{1} << window_log_for(output.size() + impl_->size))) {
        return std::unexpected(
            Error{ErrorCode::resource_limit, "zstd window is larger than dictionary + block"});
    }
    auto *context = impl_->context.get();
    if (!zstd_ok(ZSTD_DCtx_reset(context, ZSTD_reset_session_and_parameters))) {
        return std::unexpected(Error{ErrorCode::io_error, "cannot reset zstd decoder"});
    }
    const auto result = ZSTD_decompress_usingDDict(context, output.data(), output.size(),
                                                   input.data(), input.size(),
                                                   impl_->dictionary.get());
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
} // namespace seedarc::experimental::codec::research
