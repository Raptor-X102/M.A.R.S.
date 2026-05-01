#pragma once

#include <string_view>

#include "shared_types/cpu_info_data.hpp"

namespace silicon_probe::core {

class Measurer {
   public:
    virtual ~Measurer() = default;

    virtual std::string_view name() const noexcept = 0;
    virtual bool is_available() const noexcept { return true; }
    virtual void measure(shared_types::CpuInfoData& data) = 0;
};

}  // namespace silicon_probe::core
