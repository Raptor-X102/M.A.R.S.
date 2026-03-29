#include "silicon_probe/core/probe_service.hpp"

#include "silicon_probe/infra/logging.hpp"

namespace silicon_probe::core {

const CpuInfoData& ProbeService::run() {
    SPDLOG_INFO("Starting CPU measurement pipeline");
    data_ = CpuInfoData{};

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

} // namespace silicon_probe::core
