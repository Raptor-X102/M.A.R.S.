#pragma once

#include "cpu_info_data.hpp"
#include <string>

class Measurer {
public:
    virtual ~Measurer() = default;
    
    virtual void measure(CPUInfoData& data) = 0;
    virtual std::string name() const = 0;
    virtual bool is_available() const { return true; }
};
