#pragma once

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/measurer.hpp"
#include "infra/logging.hpp"

namespace silicon_probe::core {

/**
 * @brief Stores all enabled measurers for one application run.
 *
 * The registry keeps ownership of benchmark objects and skips measurers
 * that report themselves as unavailable on the current machine.
 */
class MeasurerRegistry {
   private:
    std::vector<std::unique_ptr<Measurer>> measurers_;
    std::unordered_map<std::string, Measurer*> measurer_map_;

   public:
    /**
     * @brief Adds one measurer to the registry.
     * @param measurer Benchmark object with unique ownership.
     * @throw std::invalid_argument If the pointer is null or the name is duplicated.
     */
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

        SPDLOG_DEBUG("Registered measurer: {}", key);
        measurer_map_.emplace(key, measurer.get());
        measurers_.push_back(std::move(measurer));
    }

    /**
     * @brief Returns the ordered list of registered measurers.
     * @return Immutable vector with owned measurer objects.
     */
    const std::vector<std::unique_ptr<Measurer>>& measurers() const noexcept { return measurers_; }

    /**
     * @brief Finds one measurer by name.
     * @param name Stable benchmark name.
     * @return Pointer to the measurer, or `nullptr` if the name is unknown.
     */
    const Measurer* find(std::string_view name) const noexcept {
        const auto iterator = measurer_map_.find(std::string(name));
        return iterator == measurer_map_.end() ? nullptr : iterator->second;
    }
};

}  // namespace silicon_probe::core
