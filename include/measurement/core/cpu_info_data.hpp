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

    std::optional<size_t> btb_size; // branch target buffer
    std::optional<size_t> ras_size; // return address stack
    std::optional<size_t> bht_size; // branch history table
    std::optional<size_t> uops_cache_size;
    std::optional<size_t> rob_size; // reorder buffer
    std::optional<size_t> s2l_fwd_size; // store-to-load forwarding size
    std::optional<size_t> pipeline_depth;

    std::optional<bool> execution_ports_independent; // for now only for add & mul instructions

    std::optional<size_t> cpu_family;
    std::optional<size_t> cpu_model;
    std::optional<std::string> cpu_name;
};

} // namespace silicon_probe::core
