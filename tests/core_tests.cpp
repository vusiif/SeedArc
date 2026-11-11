// SPDX-License-Identifier: GPL-3.0-or-later
#include "seedarc/checked_arithmetic.hpp"
#include "seedarc/version.hpp"

#include <iostream>
#include <limits>

int main() {
    static_assert(__cplusplus >= 202302L, "SeedArc requires C++23");
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    int failures = 0;
    const auto check = [&failures](bool condition, const char *description) {
        if (!condition) {
            std::cerr << "FAIL: " << description << '\n';
            ++failures;
        }
    };
    const auto value_is = [](const auto &result, std::uint64_t expected) {
        return result.has_value() && *result == expected;
    };
    const auto is_overflow = [](const auto &result) {
        return !result.has_value() &&
               result.error().code == seedarc::ErrorCode::arithmetic_overflow &&
               !result.error().message.empty();
    };

    check(!seedarc::version().empty(), "linked core exposes version");
    check(value_is(seedarc::checked_add(0, 0), 0), "add zero");
    check(value_is(seedarc::checked_add(maximum, 0), maximum), "add maximum");
    check(value_is(seedarc::checked_add(maximum - 1, 1), maximum), "add boundary");
    check(is_overflow(seedarc::checked_add(maximum, 1)), "reject add overflow");
    check(is_overflow(seedarc::checked_add(1, maximum)), "reject reverse add overflow");
    check(value_is(seedarc::checked_multiply(0, maximum), 0), "multiply zero");
    check(value_is(seedarc::checked_multiply(maximum, 0), 0), "reverse multiply zero");
    check(value_is(seedarc::checked_multiply(maximum, 1), maximum), "multiply maximum");
    check(value_is(seedarc::checked_multiply(maximum / 2, 2), maximum - 1), "multiply boundary");
    check(is_overflow(seedarc::checked_multiply(maximum / 2 + 1, 2)), "reject boundary overflow");
    check(is_overflow(seedarc::checked_multiply(2, maximum)), "reject multiply overflow");
    check(is_overflow(seedarc::checked_multiply(maximum, maximum)), "reject maximum product");
    return failures == 0 ? 0 : 1;
}
