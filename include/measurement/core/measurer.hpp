#pragma once

#include "measurement/core/cpu_info_data.hpp"

#include <string_view>

namespace silicon_probe::core {

class Measurer {
public:
    virtual ~Measurer() = default;

    virtual std::string_view name() const noexcept = 0;
    virtual bool is_available() const noexcept {
        return true;
    }
    virtual void measure(CpuInfoData& data) = 0;
};

} // namespace silicon_probe::core
