// SPDX-License-Identifier: GPL-3.0-or-later
#include "seedarc/checked_arithmetic.hpp"

#include <limits>

namespace seedarc {
std::expected<std::uint64_t, Error> checked_add(std::uint64_t left, std::uint64_t right) {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        return std::unexpected(Error{ErrorCode::arithmetic_overflow, "uint64 addition overflow"});
    }
    return left + right;
}

std::expected<std::uint64_t, Error> checked_multiply(std::uint64_t left, std::uint64_t right) {
    if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left) {
        return std::unexpected(
            Error{ErrorCode::arithmetic_overflow, "uint64 multiplication overflow"});
    }
    return left * right;
}
} // namespace seedarc
