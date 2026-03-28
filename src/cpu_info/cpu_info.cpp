#include "cpu_info.hpp"

CPUInfo::CPUInfo() : measured_(false) {
    auto cache_meas = std::make_unique<CacheMeasurer>();
    measurer_map_["CacheMeasurer"] = cache_meas.get();
    measurers_.push_back(std::move(cache_meas));
}

void CPUInfo::register_measurer(std::unique_ptr<Measurer> measurer) {
    if (measurer->is_available()) {
        LOG_INFO_STREAM << "Registered measurer: " << measurer->name();
        measurer_map_[measurer->name()] = measurer.get();
        measurers_.push_back(std::move(measurer));
    }
}

void CPUInfo::measure_all() {
    LOG_INFO("Starting full CPU measurement...");
    
    for (auto& measurer : measurers_) {
        try {
            LOG_INFO_STREAM << "Running: " << measurer->name();
            measurer->measure(data_);
        } catch (const std::exception& e) {
            LOG_ERROR_STREAM << measurer->name() << " failed: " << e.what();
        }
    }
    
    measured_ = true;
    LOG_INFO("CPU measurement complete");
}

void CPUInfo::measure(const std::string& name) {
    auto it = measurer_map_.find(name);
    if (it != measurer_map_.end()) {
        it->second->measure(data_);
    } else {
        LOG_WARNING_STREAM << "Measurer not found: " << name;
    }
}

void CPUInfo::measure_l1() {
    auto it = measurer_map_.find("CacheMeasurer");
    if (it != measurer_map_.end()) {
        auto* cache_meas = dynamic_cast<CacheMeasurer*>(it->second);
        if (cache_meas) {
            cache_meas->measure_l1(data_);
        }
    }
}

void CPUInfo::measure_l2() {
    auto it = measurer_map_.find("CacheMeasurer");
    if (it != measurer_map_.end()) {
        auto* cache_meas = dynamic_cast<CacheMeasurer*>(it->second);
        if (cache_meas) {
            cache_meas->measure_l2(data_);
        }
    }
}

void CPUInfo::measure_l3() {
    auto it = measurer_map_.find("CacheMeasurer");
    if (it != measurer_map_.end()) {
        auto* cache_meas = dynamic_cast<CacheMeasurer*>(it->second);
        if (cache_meas) {
            cache_meas->measure_l3(data_);
        }
    }
}

const CPUInfoData& CPUInfo::get_data() const {
    return data_;
}

void CPUInfo::print_summary() const {
    std::cout << "\n=== CPU Info Summary ===" << std::endl;
    
    if (data_.l1d_size)
        std::cout << "L1d: " << *data_.l1d_size << " bytes" << std::endl;
    if (data_.l2_size)
        std::cout << "L2:  " << *data_.l2_size << " bytes" << std::endl;
    if (data_.l3_size)
        std::cout << "L3:  " << *data_.l3_size << " bytes" << std::endl;
    if (data_.cache_line_size)
        std::cout << "Cache line: " << *data_.cache_line_size << " bytes" << std::endl;
    
    std::cout << "========================\n" << std::endl;
}
