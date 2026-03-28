#pragma once

#include "cpu_info_data.hpp"
#include "logger.hpp"
#include "measurer.hpp"
#include "cache_measurer.hpp"
#include <memory>
#include <vector>
#include <string>
#include <unordered_map>

class CPUInfo {
public:
    CPUInfo();
    
    void register_measurer(std::unique_ptr<Measurer> measurer);
    
    void measure_all();
    void measure(const std::string& name);
    
    void measure_l1();
    void measure_l2();
    void measure_l3();
    
    const CPUInfoData& get_data() const;
    void print_summary() const;

private:
    CPUInfoData data_;
    std::vector<std::unique_ptr<Measurer>> measurers_;
    std::unordered_map<std::string, Measurer*> measurer_map_;
    bool measured_;
};
