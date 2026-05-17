#pragma once

#include <optional>
#include <random>
#include <vector>
#include "core/measurer.hpp"
#include "platform/os.hpp"

namespace silicon_probe::branch_history_table {

class BranchHistoryTableMeasurer final : public core::Measurer {
public:
    static constexpr size_t      kDefaultMinPeriod   = 16;
    static constexpr size_t      kDefaultMaxPeriod   = 32 * 1024;
    static constexpr double      kDefaultPeriodCoef  = 2.0;
    static constexpr size_t      kDefaultIterations  = 100'000'000;
    static constexpr unsigned int kPatternSeed       = 123;

    struct Config {
        bool enabled = true;
        platform::MeasurementEnvironmentOptions environment;
        size_t   min_period   = kDefaultMinPeriod;
        size_t   max_period   = kDefaultMaxPeriod;
        double   period_coeff = kDefaultPeriodCoef;
        size_t   iterations   = kDefaultIterations;
        double   abs_threshold = 0.2;       // absolute miss rate threshold for fallback
        double   max_delta_mult = 0.0;      // unused, kept for future; use 0 for automatic
    };

private:
    struct BranchHistoryTableResult {
        size_t period;
        double miss_per_iter;
    };

    Config config_;

public:
    BranchHistoryTableMeasurer();
    explicit BranchHistoryTableMeasurer(Config config);

    std::string_view name() const noexcept override;
    void measure(shared_types::CpuInfoData& data) override;

private:
    void validateConfig();
    std::optional<int> detectBHTSaturation(const std::vector<BranchHistoryTableResult>& results) const;
};

} // namespace silicon_probe::branch_history_table
