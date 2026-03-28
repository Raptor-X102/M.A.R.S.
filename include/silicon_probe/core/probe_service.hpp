#pragma once

#include "silicon_probe/core/cpu_info_data.hpp"
#include "silicon_probe/core/measurer_registry.hpp"

namespace silicon_probe::core {

class ProbeService {
public:
    explicit ProbeService(MeasurerRegistry registry);

    const CpuInfoData& run();
    const CpuInfoData& data() const noexcept;

private:
    MeasurerRegistry registry_;
    CpuInfoData data_;
    bool measured_ = false;
};

} // namespace silicon_probe::core
