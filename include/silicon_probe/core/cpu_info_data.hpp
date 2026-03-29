#pragma once

#include <cstddef>
#include <optional>
#include <string>

namespace silicon_probe::core {

struct CpuInfoData {
    std::optional<size_t> l1d_size;
    std::optional<size_t> l1i_size;
    std::optional<size_t> l2_size;
    std::optional<size_t> l3_size;
    std::optional<size_t> cache_line_size;
    std::optional<bool> is_inclusive_cache;

    std::optional<size_t> tlb_l1_size;
    std::optional<size_t> tlb_l2_size;

    std::optional<size_t> btb_size;
    std::optional<size_t> ras_size;
    std::optional<size_t> bp_history_size;
    std::optional<size_t> microop_cache_size;

    std::optional<size_t> rob_size;
    std::optional<size_t> reorder_buffer_size;
    std::optional<size_t> pipeline_depth;

    std::optional<size_t> cpu_family;
    std::optional<size_t> cpu_model;
    std::optional<std::string> cpu_name;
};

} // namespace silicon_probe::core
