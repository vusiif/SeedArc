// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "seedarc/error.hpp"

#include <cstdint>
#include <expected>

namespace seedarc {
[[nodiscard]] std::expected<std::uint64_t, Error> checked_add(std::uint64_t left,
                                                              std::uint64_t right);
[[nodiscard]] std::expected<std::uint64_t, Error> checked_multiply(std::uint64_t left,
                                                                   std::uint64_t right);
} // namespace seedarc
