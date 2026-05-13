// measurement/return_address_stack/return_address_stack_measurer.hpp
#pragma once

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

#include "core/measurer.hpp"
#include "infra/logging.hpp"
#include "platform/arch.hpp"
#include "platform/os.hpp"

namespace silicon_probe::return_address_stack {

class ReturnAddressStackMeasurer final : public core::Measurer {
   public:
    static constexpr size_t kDefaultMinRecursion  = 1;
    static constexpr size_t kDefaultMaxRecursion  = 32;
    static constexpr size_t kDefaultRecursionStep = 1;
    static constexpr size_t kDefaultIterations    = 100'000'000;

    struct Config {
        bool enabled = true;
        platform::MeasurementEnvironmentOptions environment;
        size_t min_recursion_depth  = kDefaultMinRecursion;
        size_t max_recursion_depth  = kDefaultMaxRecursion;
        size_t recursion_depth_step = kDefaultRecursionStep;
        size_t iterations           = kDefaultIterations;
        double trim_ratio           = 0.02;  // trimming outliers before averaging

        // --- RAS detection parameters ---
        size_t smoothing_window       = 3;     // median filter window (odd number)
        double noise_estimation_ratio = 0.5;   // fraction of deltas used for noise estimation (0..1)
        double threshold_multiplier   = 5.0;   // multiplier for MAD: threshold = median + k * MAD
        size_t sustained_window       = 3;     // number of points after jump to check for stability
        double sustained_ratio        = 1.15;  // required after_avg / before_avg to accept jump
    };

    ReturnAddressStackMeasurer();
    explicit ReturnAddressStackMeasurer(Config config);

    std::string_view name() const noexcept override;
    void measure(shared_types::CpuInfoData& data) override;

   private:
    Config config_;

    struct Result {
        size_t depth;
        size_t min_exec_time;
        double avg_exec_time;
        size_t max_exec_time;
    };

    __attribute__((noinline, noclone)) static void recursive_func(size_t depth, size_t iteration);
    int detectRASSaturation(const std::vector<Result>& results) const;
};

}  // namespace silicon_probe::return_address_stack
