// SPDX-License-Identifier: GPL-3.0-or-later
#include "seedarc/version.hpp"
#include "seedarc/version_config.hpp"

namespace seedarc {
std::string_view version() noexcept { return SEEDARC_PROJECT_VERSION; }
} // namespace seedarc
