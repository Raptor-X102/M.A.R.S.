#pragma once

#include "silicon_probe/core/measurer.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace silicon_probe::core {

class MeasurerRegistry {
public:
    void register_measurer(std::unique_ptr<Measurer> measurer);

    const std::vector<std::unique_ptr<Measurer>>& measurers() const noexcept;
    const Measurer* find(std::string_view name) const noexcept;

private:
    std::vector<std::unique_ptr<Measurer>> measurers_;
    std::unordered_map<std::string, Measurer*> measurer_map_;
};

} // namespace silicon_probe::core
