#include "silicon_probe/core/measurer_registry.hpp"

#include "silicon_probe/infra/logging.hpp"

#include <stdexcept>

namespace silicon_probe::core {

void MeasurerRegistry::register_measurer(std::unique_ptr<Measurer> measurer) {
    if (!measurer) {
        throw std::invalid_argument("Cannot register a null measurer");
    }

    const std::string key(measurer->name());
    if (measurer_map_.count(key) != 0U) {
        throw std::invalid_argument("Measurer already registered: " + key);
    }

    if (!measurer->is_available()) {
        SPDLOG_WARN("Skipping unavailable measurer: {}", key);
        return;
    }

    SPDLOG_INFO("Registered measurer: {}", key);
    measurer_map_.emplace(key, measurer.get());
    measurers_.push_back(std::move(measurer));
}

} // namespace silicon_probe::core
