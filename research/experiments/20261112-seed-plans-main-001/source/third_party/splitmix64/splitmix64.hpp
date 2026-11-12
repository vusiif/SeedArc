// Adapted from Sebastiano Vigna's 2015 public-domain SplitMix implementation.
// Original dedication, permission and disclaimer: LICENSE. Adaptation: UPSTREAM.md.
#pragma once

#include <cstdint>

namespace seedarc::third_party {
// One SplitMix64 output step from an explicit state value, without mutating global state.
[[nodiscard]] inline std::uint64_t splitmix64_step(std::uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}
} // namespace seedarc::third_party
