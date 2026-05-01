#pragma once

#include <iomanip>
#include <ostream>

#include "shared_types/cpu_info_data.hpp"

namespace silicon_probe::core {

class SummaryPrinter {
   public:
    static void print(std::ostream& stream, const shared_types::CpuInfoData& data) {
        stream << "\n=== CPU Info Summary ===\n";

        if (data.cpu_vendor) {
            stream << "CPU vendor: " << data.cpu_vendor->name() << '\n';
        }
        if (data.l1d_size) {
            stream << "L1d: " << *data.l1d_size << " bytes\n";
        }
        if (data.l2_size) {
            stream << "L2:  " << *data.l2_size << " bytes\n";
        }
        if (data.l3_size) {
            stream << "L3:  " << *data.l3_size << " bytes\n";
        }
        if (data.cache_line_size) {
            stream << "Cache line: " << *data.cache_line_size << " bytes\n";
        }
        if (data.tlb_l1_size) {
            stream << "L1 DTLB estimate: " << *data.tlb_l1_size << " pages";
            if (data.tlb_page_size_bytes) {
                stream << " (~" << (*data.tlb_l1_size * *data.tlb_page_size_bytes) << " bytes coverage)";
            }
            stream << '\n';
        }
        if (data.tlb_l2_size) {
            stream << "L2/STLB estimate: " << *data.tlb_l2_size << " pages";
            if (data.tlb_page_size_bytes) {
                stream << " (~" << (*data.tlb_l2_size * *data.tlb_page_size_bytes) << " bytes coverage)";
            }
            stream << '\n';
        }
        if (data.rob_size) {
            stream << "Rob size: " << *data.rob_size << " instructions\n";
        }
        if (data.bht_size) {
            stream << "Branch History Table size: " << *data.bht_size << " entries\n";
        }
        if (data.ras_size) {
            stream << "Return Address Stack size: " << *data.ras_size << " entries\n";
        }
        if (data.execution_ports_independent) {
            stream << "Execution ports independent: " << *data.execution_ports_independent << "\n";
        }
        if (data.uops_cache_size) {
            stream << "Uops cache size: " << *data.uops_cache_size << " uops\n";
        }
        if (data.btb_size) {
            stream << "BTB size: " << *data.btb_size << " addresses\n";
        }
        if (data.s2l_fwd_max_size) {
            stream << "Store-to-load forwarding size: " << *data.s2l_fwd_max_size << " bytes\n";
        }
        if (data.s2l_fwd_max_offset) {
            stream << "Store-to-load forwarding size: " << *data.s2l_fwd_max_offset << " bytes\n";
        }
        if (data.write_buffer_size) {
            stream << "Write buffer size: " << *data.write_buffer_size << " entries\n";
        }

        stream << "========================\n\n";
    }
};

}  // namespace silicon_probe::core
