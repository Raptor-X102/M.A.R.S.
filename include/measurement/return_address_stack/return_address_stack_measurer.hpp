// measurement/return_address_stack/return_address_stack_measurer.hpp
#pragma once

#include <vector>

#include "core/measurer.hpp"
#include "infra/logging.hpp"
#include "platform/arch.hpp"
#include "platform/os.hpp"

namespace silicon_probe::return_address_stack {

/**
 * @brief Measures return address stack size.
 *
 * This benchmark uses deeper and deeper recursion.
 * It looks for the first stable jump in time.
 */
class ReturnAddressStackMeasurer final : public core::Measurer {
   public:
    static constexpr size_t kDefaultMinRecursion  = 1;
    static constexpr size_t kDefaultMaxRecursion  = 32;
    static constexpr size_t kDefaultRecursionStep = 1;
    static constexpr size_t kDefaultIterations    = 10'000;
    static constexpr size_t kMaxSafeRecursionDepth = 64;

    /** @brief Settings for the RAS benchmark. */
    struct Config {
        bool   enabled                        = true;                 ///< Turn this measurer on or off.
        platform::MeasurementEnvironmentOptions environment;          ///< CPU and scheduler settings for the run.
        size_t min_recursion_depth            = kDefaultMinRecursion; ///< Smallest recursion depth to test.
        size_t max_recursion_depth            = kDefaultMaxRecursion; ///< Largest recursion depth to test.
        size_t recursion_depth_step           = kDefaultRecursionStep;///< Step between recursion depths.
        size_t iterations                     = kDefaultIterations;   ///< Number of timing samples for one depth.
        double trim_ratio                     = 0.02;                 ///< Part of extreme samples to remove.

        size_t smoothing_window               = 3;                    ///< Median-window size before jump search.
        double noise_estimation_ratio         = 0.5;                  ///< Reserved for future logic.
        double threshold_multiplier           = 5.0;                  ///< Reserved for future logic.
        size_t sustained_window               = 3;                    ///< Reserved for future logic.
        double sustained_ratio                = 1.15;                 ///< Min time ratio after the jump.
    };

    /** @brief Builds the measurer with default settings. */
    ReturnAddressStackMeasurer();

    /**
     * @brief Builds the measurer with custom settings.
     * @param config Settings for the next runs.
     */
    explicit ReturnAddressStackMeasurer(Config config);

    /**
     * @brief Returns the benchmark name.
     * @return Name used in logs and output.
     */
    std::string_view name() const noexcept override;

    /**
     * @brief Runs the RAS benchmark.
     * @param data Output data. The function writes the RAS size here.
     */
    void measure(shared_types::CpuInfoData& data) override;

   private:
    Config config_;

    /** @brief Result for one recursion depth. */
    struct Result {
        size_t depth;          ///< Tested recursion depth.
        double avg_exec_time;  ///< Average time for this depth.
    };

    /** @brief Fixes bad setting values before the run. */
    void validateConfig();

    /**
     * @brief Recursive function used in the timing test.
     * @param depth Target recursion depth.
     * @param iteration Current recursion level.
     */
    __attribute__((noinline, noclone, noipa)) static void recursive_func(size_t depth, size_t iteration);

    /**
     * @brief Finds the first stable jump in the sweep.
     * @param results Measured points in order.
     * @return Estimated RAS size, or a negative value if no clear jump was found.
     */
    int detectRASSaturation(const std::vector<Result>& results) const;
};

}  // namespace silicon_probe::return_address_stack
