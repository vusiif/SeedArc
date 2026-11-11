// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

namespace seedarc {
enum class ErrorCode {
    invalid_argument,
    arithmetic_overflow,
    not_implemented,
    malformed_archive,
    unsupported_feature,
    resource_limit,
    unsafe_path,
    io_error,
    output_conflict,
    integrity_error
};

struct Error {
    ErrorCode code;
    std::string message;
};
} // namespace seedarc
