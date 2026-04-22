#pragma once

#include <string>
#include <vector>
#include <optional>

namespace silicon_probe::platform {

// Automatically discover available port events for the current CPU.
// Returns a list of event names that can be used with libpfm4.
std::vector<std::string> discover_port_events();
std::vector<std::string> discover_uops_events();
std::optional<std::string> discover_branch_target_buffer_events();

} // namespace silicon_probe::platform
