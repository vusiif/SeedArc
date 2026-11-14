// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Internal to libseedarc and its tests; not installed and not part of the public API.

#include "seedarc/error.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <limits>
#include <string_view>

namespace seedarc::experimental::internal {
// Cumulative budget check for adding one component of `component_units` UTF-16 units below a
// directory at `depth` whose equivalent DOS path has `path_units` units (see safe_output.hpp).
[[nodiscard]] std::expected<void, Error> admit_output_component(std::size_t depth,
                                                                std::size_t path_units,
                                                                std::size_t component_units,
                                                                bool directory) noexcept;

// True only for a canonical volume GUID root such as "\\?\Volume{...}\". A drive letter created by
// SUBST or a redirected device resolves to a longer path and is rejected as an output root.
[[nodiscard]] bool is_volume_guid_root(std::wstring_view final_path) noexcept;

// Test seam for write_new_file failure paths. Production code never installs faults; the hook is
// thread-local and consulted only by the thread that installed it.
struct WriteFaults {
    // Report at most this many bytes written per WriteFile call (0 = no cap): a short write.
    std::size_t max_bytes_per_write = 0;
    // Report a WriteFile failure (ERROR_DISK_FULL) once this many bytes have been written.
    std::uint64_t fail_write_at = std::numeric_limits<std::uint64_t>::max();
    bool fail_flush = false;
    bool corrupt_read_back = false;
    bool fail_commit = false;
    // Make the explicit cleanup in the failure path report an error; close-time cleanup still runs.
    bool fail_explicit_cleanup = false;
    // Called after each successful write call with the total bytes written so far.
    std::function<void(std::uint64_t)> after_write;
    // Linux only: publish O_TMPFILE files through /proc/self/fd instead of AT_EMPTY_PATH, so that
    // the fallback used before Linux 6.10 (without CAP_DAC_READ_SEARCH) is tested too.
    bool force_proc_link = false;
};

class ScopedWriteFaults {
  public:
    explicit ScopedWriteFaults(const WriteFaults &faults) noexcept;
    ~ScopedWriteFaults();
    ScopedWriteFaults(const ScopedWriteFaults &) = delete;
    ScopedWriteFaults &operator=(const ScopedWriteFaults &) = delete;

  private:
    const WriteFaults *previous_;
};

[[nodiscard]] const WriteFaults *active_write_faults() noexcept;
} // namespace seedarc::experimental::internal
