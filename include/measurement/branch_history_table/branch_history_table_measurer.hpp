#pragma once

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>
#include "platform/os.hpp"
#include "core/measurer.hpp"

namespace silicon_probe::branch_history_table {

class BranchHistoryTableMeasurer final : public core::Measurer {
public:
    static constexpr size_t kDefaultMinPeriod = 16;
    static constexpr size_t kDefaultMaxPeriod = 32 * 1024;
    static constexpr size_t kDefaultPeriodCoef = 2;
    static constexpr size_t kDefaultIterations = 100'000'000;
    static constexpr unsigned int kPatternSeed = 123;
    static constexpr double kThreshold035 = 0.35;
    static constexpr double kThreshold040 = 0.40;
    static constexpr double kBaselineFraction = 1.0 / 3.0;
    static constexpr size_t kBaselineMaxSamples = 4;
    static constexpr double kBaselineOffset = 0.15;
    static constexpr double kMinDelta = 0.05;

    struct Config {
        bool enabled = true;
        platform::MeasurementEnvironmentOptions environment;
        size_t min_period = kDefaultMinPeriod;
        size_t max_period = kDefaultMaxPeriod;
        size_t period_coeff = kDefaultPeriodCoef;
        size_t iterations = kDefaultIterations;
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
    struct BHTEstimates {
        int baseline_offset = -1;
        int max_delta = -1;
        int inflection_point = -1;
    };

    int detectBHTSaturation(const std::vector<BranchHistoryTableResult>& results) const;
};

}  // namespace silicon_probe::branch_history_table
