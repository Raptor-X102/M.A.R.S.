#include "silicon_probe/platform/cpu_vendor.hpp"
#include "silicon_probe/infra/logging.hpp"
#include <perfmon/pfmlib.h>
#include <perfmon/pfmlib_perf_event.h>

namespace silicon_probe::platform::cpu_vendor {

static void ensure_pfm_initialized() {
    static std::once_flag flag;
    std::call_once(flag, []() {
        static const bool pfm_initialized = []() {
            return pfm_initialize() == PFM_SUCCESS;
        }();
        
        if (!pfm_initialized) {
            SPDLOG_ERROR("libpfm4 initialization failed");
        }
    });
}

static bool is_event_supported(const std::string& event_name) {
    ensure_pfm_initialized();
    
    int idx = pfm_find_event(event_name.c_str());
    if (idx < 0) {
        return false;
    }
    
    pfm_event_info_t info{};
    info.size = sizeof(info);
    
    int ret = pfm_get_event_info(idx, PFM_OS_NONE, &info);
    return (ret == PFM_SUCCESS);
}

CpuEvents map_events_names(const CpuVendor& vendor) {
    using ID = CpuVendor::CpuVendorID;
    
    if (vendor == ID::Intel) {
        ensure_pfm_initialized();

        std::string issued = "uops_issued.any";
        std::string retired = "inst_retired.any";
        std::string stalls = "cycle_activity.stalls_total";

        if (!is_event_supported(issued)) {
            issued = "";
        }
        
        if (!is_event_supported(retired)) {
            retired = "";
        }

        if (!is_event_supported(stalls)) {
            stalls = "";
        }
        
        return {issued, retired, stalls};
    }
    
    SPDLOG_ERROR("Unsupported CPU vendor: {}", vendor.name());
    return {};
}

} // namespace silicon_probe::platform::cpu_vendor
