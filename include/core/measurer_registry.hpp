#pragma once

#include "core/measurer.hpp"
#include "infra/logging.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace silicon_probe::core {

class MeasurerRegistry {
private:
    std::vector<std::unique_ptr<Measurer>> measurers_;
    std::unordered_map<std::string, Measurer*> measurer_map_;

public:
    void register_measurer(std::unique_ptr<Measurer> measurer) {
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

    const std::vector<std::unique_ptr<Measurer>>& measurers() const noexcept {
        return measurers_;
    }

    const Measurer* find(std::string_view name) const noexcept {
        const auto iterator = measurer_map_.find(std::string(name));
        return iterator == measurer_map_.end() ? nullptr : iterator->second;
    }
};

} // namespace silicon_probe::core
