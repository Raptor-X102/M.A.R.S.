#pragma once

#include "silicon_probe/core/cpu_info_data.hpp"

#include <ostream>

namespace silicon_probe::core {

class SummaryPrinter {
public:
    static void print(std::ostream& stream, const CpuInfoData& data) {
        stream << "\n=== CPU Info Summary ===\n";

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
        if (data.rob_size) {
            stream << "Rob size: " << *data.rob_size << " instructions\n";
        }

        stream << "========================\n\n";
    }
};

} // namespace silicon_probe::core
