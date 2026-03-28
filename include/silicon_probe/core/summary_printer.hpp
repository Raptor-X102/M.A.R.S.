#pragma once

#include "silicon_probe/core/cpu_info_data.hpp"

#include <iosfwd>

namespace silicon_probe::core {

class SummaryPrinter {
public:
    static void print(std::ostream& stream, const CpuInfoData& data);
};

} // namespace silicon_probe::core
