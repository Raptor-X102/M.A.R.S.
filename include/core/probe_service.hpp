#pragma once

#include <exception>
#include <utility>

#include "core/measurer_registry.hpp"
#include "infra/logging.hpp"
#include "shared_types/cpu_info_data.hpp"

namespace silicon_probe::core {

class ProbeService {
   private:
    MeasurerRegistry registry_;
    shared_types::CpuInfoData data_;
    bool measured_ = false;

   public:
    explicit ProbeService(MeasurerRegistry registry) : registry_(std::move(registry)) {}

    const shared_types::CpuInfoData& run() {
        SPDLOG_INFO("Starting CPU measurement pipeline");
        data_ = shared_types::CpuInfoData{};

        for (const auto& measurer : registry_.measurers()) {
            try {
                SPDLOG_INFO("Running measurer: {}", measurer->name());
                measurer->measure(data_);
            } catch (const std::exception& error) {
                SPDLOG_ERROR("Measurer '{}' failed: {}", measurer->name(), error.what());
            }
        }

        measured_ = true;
        SPDLOG_INFO("CPU measurement pipeline complete");
        return data_;
    }

    const shared_types::CpuInfoData& data() const noexcept { return data_; }
};

}  // namespace silicon_probe::core
