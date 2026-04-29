#include "platform/events_discovery.hpp"
#include "infra/logging.hpp"

namespace silicon_probe::platform {

static bool ensure_pfm(shared_types::CpuInfoData& data) {
    static std::once_flag flag;
    static bool initialized = false;
    std::call_once(flag, [&data]() {
        initialized = (pfm_initialize() == PFM_SUCCESS);
        if (!initialized) {
            SPDLOG_ERROR("libpfm4 initialization failed");
        }
        data.cpu_vendor = arch::detect_vendor();
    });
    return initialized;
}

static inline bool event_exists(const std::string& name) {
    //if (!ensure_pfm()) return false;
    return pfm_find_event(name.c_str()) >= 0;
}

std::vector<std::string> discover_port_events(shared_types::CpuInfoData& data) {
    if (!ensure_pfm(data)) return {};

    using CpuVendor = silicon_probe::platform::cpu_vendor::CpuVendor;
    std::vector<std::string> candidates;

    if (data.cpu_vendor == CpuVendor::CpuVendorID::Intel) {
        // Try modern names first (uops_dispatched.port_X)
        for (int port = 0; port <= 7; ++port) {
            std::string name = "uops_dispatched.port_" + std::to_string(port);
            if (event_exists(name))
                candidates.push_back(name);
        }

        // Load ports (grouped)
        std::vector<std::string> load_port_names = {
            "uops_dispatched.port_2_3_10",
            "uops_dispatched.port_2_3",
            "uops_dispatched.port_2",
            "uops_dispatched.port_3"
        };
        for (const auto& name : load_port_names) {
            if (event_exists(name)) {
                candidates.push_back(name);
                break; // take first available
            }
        }
        // Store ports
        std::vector<std::string> store_port_names = {
            "uops_dispatched.port_4_9",
            "uops_dispatched.port_4",
            "uops_dispatched.port_7_8",
            "uops_dispatched.port_7"
        };
        for (const auto& name : store_port_names) {
            if (event_exists(name)) {
                candidates.push_back(name);
                break;
            }
        }
        // If none found, try older naming (UOPS_DISPATCHED_PORT.PORT_X)
        if (candidates.empty()) {
            for (int port = 0; port <= 7; ++port) {
                std::string name = "UOPS_DISPATCHED_PORT.PORT_" + std::to_string(port);
                if (event_exists(name))
                    candidates.push_back(name);
            }
        }
        // Fallback to lowercase alternative
        if (candidates.empty()) {
            for (int port = 0; port <= 7; ++port) {
                std::string name = "uops_dispatched_port.port_" + std::to_string(port);
                if (event_exists(name))
                    candidates.push_back(name);
            }
        }
    }
   // else if (vendor == CpuVendor::CpuVendorID::AMD) {
   //     for (int port = 0; port <= 3; ++port) {
   //         std::string name = "EX_RET_UOPS_RETIRE.PORTS_" + std::to_string(port);
   //         if (event_exists(name))
   //             candidates.push_back(name);
   //         else {
   //             name = "ex_ret_uops_retire.ports_" + std::to_string(port);
   //             if (event_exists(name))
   //                 candidates.push_back(name);
   //         }
   //     }
   //     // fallback: aggregated port event
   //     if (candidates.empty() && event_exists("EX_RET_UOPS_RETIRE.PORTS")) {
   //         candidates.push_back("EX_RET_UOPS_RETIRE.PORTS");
   //     }
   // }
    else {
        if (data.cpu_vendor)
            SPDLOG_WARN("Unsupported CPU vendor ({}) for port events discovery", data.cpu_vendor->name());
    }

    return candidates;
}

std::vector<std::string> discover_uops_events(shared_types::CpuInfoData& data) {
    if (!ensure_pfm(data)) return {};

    using CpuVendor = silicon_probe::platform::cpu_vendor::CpuVendor;

    std::vector<std::string> candidates;

    if (data.cpu_vendor == CpuVendor::CpuVendorID::Intel) {
        const std::string MITE_UOPS = "idq.mite_uops";
        const std::string DSB_UOPS = "idq.dsb_uops";
        if (event_exists(MITE_UOPS) && event_exists(DSB_UOPS)) {
            candidates.push_back(MITE_UOPS);
            candidates.push_back(DSB_UOPS);
        }
    }
    else {
        if (data.cpu_vendor)
            SPDLOG_WARN("Unsupported CPU vendor ({}) for uops event discovery", data.cpu_vendor->name());
    }

    return candidates;
}

std::optional<std::string> discover_branch_target_buffer_events(shared_types::CpuInfoData& data) {
    if (!ensure_pfm(data)) return {};

    using CpuVendor = silicon_probe::platform::cpu_vendor::CpuVendor;

    if (data.cpu_vendor == CpuVendor::CpuVendorID::Intel) {
        std::string br_misp_retired_inderect = "br_misp_retired.indirect";
        if (event_exists(br_misp_retired_inderect )) {
            return br_misp_retired_inderect;
        }
    }
    else {
        if (data.cpu_vendor)
            SPDLOG_WARN("Unsupported CPU vendor ({}) for branch target buffer events discovery", data.cpu_vendor->name());
    }

    return std::nullopt;
}

std::vector<std::string> discover_s2l_forwarding_events(shared_types::CpuInfoData& data) {
    if (!ensure_pfm(data)) return {};

    using CpuVendor = silicon_probe::platform::cpu_vendor::CpuVendor;
    std::vector<std::string> candidates;

    if (data.cpu_vendor == CpuVendor::CpuVendorID::Intel) {
        const std::string STORE_FORWARD = "ld_blocks.store_forward";
        if (event_exists(STORE_FORWARD) /*&& event_exists(SPLIT_STORES)*/) {
            candidates.push_back(STORE_FORWARD);
        }
    }
    else {
        if (data.cpu_vendor)
            SPDLOG_WARN("Unsupported CPU vendor ({}) for store to load forwarding events discovery", data.cpu_vendor->name());
    }

    return candidates;
}

std::vector<std::string> discover_write_buffer_events(shared_types::CpuInfoData& data) {
    if (!ensure_pfm(data)) return {};

    using CpuVendor = silicon_probe::platform::cpu_vendor::CpuVendor;
    std::vector<std::string> candidates;

    if (data.cpu_vendor == CpuVendor::CpuVendorID::Intel) {
        const std::string RESOURCE_STALLS = "resource_stalls.sb";
        const std::string BOUND_ON_STORES = "exe_activity.bound_on_stores";
        //const std::string MEM_BOUND_STALLS_LOAD = "mem_bound_stalls.load";
        if (event_exists(RESOURCE_STALLS)) {
            candidates.push_back(RESOURCE_STALLS);
        }
        if (event_exists(BOUND_ON_STORES)) {
            candidates.push_back(BOUND_ON_STORES);
        }
        /*if (event_exists(MEM_BOUND_STALLS_LOAD)) {
            candidates.push_back(MEM_BOUND_STALLS_LOAD);
        }*/
    }
    else {
        if (data.cpu_vendor)
            SPDLOG_WARN("Unsupported CPU vendor ({}) for write buffer events discovery", data.cpu_vendor->name());
    }

    return candidates;
}

using CacheLevel = silicon_probe::shared_types::CacheLevel;

std::vector<std::string> discover_cache_miss_events(CacheLevel level, shared_types::CpuInfoData& data) {
    if (!ensure_pfm(data)) return {};

    using CpuVendor = silicon_probe::platform::cpu_vendor::CpuVendor;
    std::vector<std::string> candidates;

    if (data.cpu_vendor == CpuVendor::CpuVendorID::Intel) {
        switch (level) {
            case CacheLevel::l1d:
                if (event_exists("mem_load_retired.l1_miss"))
                    candidates.push_back("mem_load_retired.l1_miss");
                if (event_exists("l1-dcache-load-misses"))
                    candidates.push_back("l1-dcache-load-misses");
                break;
            case CacheLevel::l2:
                if (event_exists("l2_rqsts.all_demand_miss"))
                    candidates.push_back("l2_rqsts.all_demand_miss");
                if (event_exists("mem_load_retired.l2_miss"))
                    candidates.push_back("mem_load_retired.l2_miss");
                break;
            case CacheLevel::l3:
                if (event_exists("longest_lat_cache.miss"))
                    candidates.push_back("longest_lat_cache.miss");
                if (event_exists("mem_load_retired.l3_miss"))
                    candidates.push_back("mem_load_retired.l3_miss");
                break;
            default:
                throw std::invalid_argument("discover_cache_miss_events: insert proper cache level");
        }
    }
    else {
        if (data.cpu_vendor)
            SPDLOG_WARN("Unsupported CPU vendor ({}) for cache misses events discovery", data.cpu_vendor->name());
    }

    return candidates;
}

} // namespace silicon_probe::platform
