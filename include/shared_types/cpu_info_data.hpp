#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "shared_types/cpu_vendor.hpp"

namespace silicon_probe::shared_types {

// Summary for one TLB test point.
struct TlbSummaryPoint {
    size_t pages                    = 0;    // Number of touched pages.
    size_t bytes                    = 0;    // Total size in bytes.
    double min_cycles_per_access    = 0.0;  // Best cycles per access.
    double median_cycles_per_access = 0.0;  // Median cycles per access.
    double mean_cycles_per_access   = 0.0;  // Average cycles per access.
    double max_cycles_per_access    = 0.0;  // Worst cycles per access.
};

// Raw TLB sample for one repeat.
struct TlbRawPoint {
    size_t pages             = 0;    // Number of touched pages.
    size_t bytes             = 0;    // Total size in bytes.
    size_t repeat            = 0;    // Repeat index for this point.
    double cycles_per_access = 0.0;  // Measured cycles per access.
};

// Shared output from all benchmarks.
struct CpuInfoData {
    std::optional<size_t> l1d_size;          // Measured L1 data cache size in bytes.
    std::optional<size_t> l1i_size;          // Measured L1 instruction cache size in bytes.
    std::optional<size_t> l2_size;           // Measured L2 cache size in bytes.
    std::optional<size_t> l3_size;           // Measured L3 cache size in bytes.
    std::optional<size_t> cache_line_size;   // Cache line size in bytes.
    std::optional<bool> is_inclusive_cache;  // `true` if the cache looks inclusive.

    std::optional<size_t> tlb_l1_size;          // Measured L1 TLB size in pages.
    std::optional<size_t> tlb_l2_size;          // Measured L2 TLB size in pages.
    std::optional<size_t> tlb_page_size_bytes;  // Page size used in the TLB test.

    std::optional<size_t> btb_size;            // Measured BTB size in entries.
    std::optional<size_t> ras_size;            // Measured RAS size in entries.
    std::optional<size_t> bht_size;            // Measured BHT size in entries.
    std::optional<size_t> uops_cache_size;     // Measured uops cache size in uops.
    std::optional<size_t> rob_size;            // Measured ROB size in entries.
    std::optional<size_t> s2l_fwd_max_size;    // Largest store size that still forwards.
    std::optional<size_t> s2l_fwd_max_offset;  // Largest offset that still forwards.
    std::optional<size_t> write_buffer_size;   // Measured write-buffer size.
    std::optional<size_t> pipeline_depth;      // Measured pipeline depth.

    std::optional<bool> execution_ports_independent;  // `true` if the tested instructions use different ports.

    std::optional<platform::cpu_vendor::CpuVendor> cpu_vendor;  // Detected CPU vendor.
};

}  // namespace silicon_probe::shared_types
