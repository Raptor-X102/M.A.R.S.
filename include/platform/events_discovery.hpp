#pragma once

#include <cstring>
#include <mutex>
#include <optional>
#include <perfmon/pfmlib.h>
#include <perfmon/pfmlib_perf_event.h>
#include <string>
#include <vector>

#include "platform/arch.hpp"
#include "shared_types/cache_types.hpp"
#include "shared_types/cpu_info_data.hpp"

namespace silicon_probe::platform {

// Automatically discover available port events for the current CPU.
// Returns a list of event names that can be used with libpfm4.
std::vector<std::string> discover_port_events(shared_types::CpuInfoData& data);
std::vector<std::string> discover_uops_events(shared_types::CpuInfoData& data);
std::optional<std::string> discover_branch_target_buffer_events(shared_types::CpuInfoData& data);
std::vector<std::string> discover_s2l_forwarding_events(shared_types::CpuInfoData& data);
std::vector<std::string> discover_write_buffer_events(shared_types::CpuInfoData& data);
std::vector<std::string> discover_cache_miss_events(silicon_probe::shared_types::CacheLevel level,
                                                    shared_types::CpuInfoData& data);

}  // namespace silicon_probe::platform
