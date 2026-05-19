#pragma once

#include <optional>
#include <random>
#include <vector>
#include "core/measurer.hpp"
#include "platform/os.hpp"

namespace silicon_probe::branch_history_table {

/**
 * @brief Measures branch history table size.
 *
 * This benchmark runs branch patterns with a growing period.
 * It looks for the point where branch misses go up.
 */
class BranchHistoryTableMeasurer final : public core::Measurer {
   public:
    static constexpr size_t kDefaultMinPeriod  = 16;
    static constexpr size_t kDefaultMaxPeriod  = 32 * 1024;
    static constexpr double kDefaultPeriodCoef = 2.0;
    static constexpr size_t kDefaultIterations = 100'000'000;
    static constexpr unsigned int kPatternSeed = 123;

    /** @brief Settings for the BHT benchmark. */
    struct Config {
        bool enabled = true;                                  ///< Turn this measurer on or off.
        platform::MeasurementEnvironmentOptions environment;  ///< CPU and scheduler settings for the run.
        size_t min_period     = kDefaultMinPeriod;            ///< Smallest pattern period to test.
        size_t max_period     = kDefaultMaxPeriod;            ///< Largest pattern period to test.
        double period_coeff   = kDefaultPeriodCoef;           ///< Growth factor between test points.
        size_t iterations     = kDefaultIterations;           ///< Number of branch steps for one test point.
        double abs_threshold  = 0.2;                          ///< Fallback miss-rate limit.
        double max_delta_mult = 0.0;                          ///< Reserved for future logic.
    };

   private:
    /** @brief Result for one pattern period. */
    struct BranchHistoryTableResult {
        size_t period;         ///< Tested pattern period.
        double miss_per_iter;  ///< Average misses per branch.
    };

    Config config_;

   public:
    /** @brief Builds the measurer with default settings. */
    BranchHistoryTableMeasurer();

    /**
     * @brief Builds the measurer with custom settings.
     * @param config Settings for the next runs.
     */
    explicit BranchHistoryTableMeasurer(Config config);

    /**
     * @brief Returns the benchmark name.
     * @return Name used in logs and output.
     */
    std::string_view name() const noexcept override;

    /**
     * @brief Runs the BHT benchmark.
     * @param data Output data. The function writes the BHT size here.
     */
    void measure(shared_types::CpuInfoData& data) override;

   private:
    /** @brief Fixes bad setting values before the run. */
    void validateConfig();

    /**
     * @brief Finds the BHT size from all measured points.
     * @param results Measured miss-rate points in order.
     * @return Estimated BHT size, or `std::nullopt` if no clear result was found.
     */
    std::optional<int> detectBHTSaturation(const std::vector<BranchHistoryTableResult>& results) const;
};

}  // namespace silicon_probe::branch_history_table
