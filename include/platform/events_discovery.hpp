#pragma once

#include <string>
#include <vector>
#include <optional>
#include "shared_types/cache_types.hpp"

namespace silicon_probe::platform {

// Automatically discover available port events for the current CPU.
// Returns a list of event names that can be used with libpfm4.
std::vector<std::string> discover_port_events();
std::vector<std::string> discover_uops_events();
std::optional<std::string> discover_branch_target_buffer_events();
std::vector<std::string> discover_s2l_forwarding_events();
std::vector<std::string> discover_write_buffer_events();
std::vector<std::string> discover_cache_miss_events(silicon_probe::shared_types::CacheLevel level);

} // namespace silicon_probe::platform
