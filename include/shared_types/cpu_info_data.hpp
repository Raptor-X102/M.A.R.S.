#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include "shared_types/cpu_vendor.hpp"

namespace silicon_probe::shared_types {

struct TlbSummaryPoint {
    size_t pages = 0;
    size_t bytes = 0;
    double min_cycles_per_access = 0.0;
    double median_cycles_per_access = 0.0;
    double mean_cycles_per_access = 0.0;
    double max_cycles_per_access = 0.0;
};

struct TlbRawPoint {
    size_t pages = 0;
    size_t bytes = 0;
    size_t repeat = 0;
    double cycles_per_access = 0.0;
};

struct CpuInfoData {
    std::optional<size_t> l1d_size;
    std::optional<size_t> l1i_size;
    std::optional<size_t> l2_size;
    std::optional<size_t> l3_size;
    std::optional<size_t> cache_line_size;
    std::optional<bool> is_inclusive_cache;

    std::optional<size_t> tlb_l1_size;
    std::optional<size_t> tlb_l2_size;
    std::optional<size_t> tlb_page_walk_threshold;
    std::optional<size_t> tlb_page_size_bytes;
    std::vector<TlbSummaryPoint> tlb_points;
    std::vector<TlbRawPoint> tlb_raw_points;

    std::optional<size_t> btb_size; // branch target buffer
    std::optional<size_t> ras_size; // return address stack
    std::optional<size_t> bht_size; // branch history table
    std::optional<size_t> uops_cache_size;
    std::optional<size_t> rob_size; // reorder buffer
    std::optional<size_t> s2l_fwd_max_size; // store-to-load forwarding max size
    std::optional<size_t> s2l_fwd_max_offset; // store-to-load forwarding max offset
    std::optional<size_t> write_buffer_size;
    std::optional<size_t> pipeline_depth;

    std::optional<bool> execution_ports_independent; // for now only for add & mul instructions

    std::optional<platform::cpu_vendor::CpuVendor> cpu_vendor;
};

} // namespace silicon_probe::core
