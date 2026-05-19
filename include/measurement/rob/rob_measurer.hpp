#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <numeric>
#include <random>
#include <vector>

#include "core/measurer.hpp"
#include "infra/logging.hpp"
#include "platform/arch.hpp"
#include "platform/os.hpp"
#include "platform/pmc.hpp"

namespace silicon_probe::rob {

/**
 * @brief Measures reorder buffer size.
 *
 * This benchmark adds more filler instructions.
 * It looks for the first stable jump in time.
 */
class RobMeasurer final : public core::Measurer {
   public:
    static constexpr size_t kDefaultMinInstrCnt      = 50;
    static constexpr size_t kDefaultMaxInstrCnt      = 600;
    static constexpr size_t kDefaultInstrCntStep     = 10;
    static constexpr size_t kDefaultWarmupIterations = 2;
    static constexpr size_t kDefaultInnerIterations  = 8192;
    static constexpr size_t kDefaultOuterIterations  = 64;
    static constexpr size_t kDefaultUnroll           = 17;
    static constexpr int kDefaultInstrType           = 4;
    static constexpr int kMinResultsCnt              = 8;

    /** @brief Settings for the ROB benchmark. */
    struct Config {
        bool   enabled                           = true;                     ///< Turn this measurer on or off.
        platform::MeasurementEnvironmentOptions environment;                 ///< CPU and scheduler settings for the run.
        size_t min_instr_cnt                     = kDefaultMinInstrCnt;     ///< Smallest filler count to test.
        size_t max_instr_cnt                     = kDefaultMaxInstrCnt;     ///< Largest filler count to test.
        size_t instr_cnt_step                    = kDefaultInstrCntStep;    ///< Step between filler counts.
        size_t warmup_iterations                 = kDefaultWarmupIterations;///< Warm-up calls before timing.
        size_t inner_iterations                  = kDefaultInnerIterations; ///< Loop count inside generated code.
        size_t outer_iterations                  = kDefaultOuterIterations; ///< Number of timed repeats for one point.
        int    instr_type                        = kDefaultInstrType;       ///< Instruction type for generated code.
        double baseline_fraction                 = 0.2;                     ///< Reserved for old baseline logic.
        size_t baseline_min_samples              = 5;                       ///< Reserved for old baseline logic.
        size_t required_consecutive_points       = 3;                       ///< Number of points that must stay high.
        double saturation_threshold_ratio        = 1.15;                    ///< Reserved for old threshold logic.
        double fallback_jump_ratio               = 0.15;                    ///< Reserved for fallback logic.
        double sustain_threshold                 = 0.9;                     ///< Allowed drop after the jump.
        size_t unroll                            = kDefaultUnroll;          ///< Unroll factor in generated code.
        int    min_res_cnt                       = kMinResultsCnt;          ///< Minimum number of sweep points.
    };

    /** @brief Builds the measurer with default settings. */
    RobMeasurer();

    /**
     * @brief Builds the measurer with custom settings.
     * @param config Settings for the next runs.
     */
    explicit RobMeasurer(Config config);

    /**
     * @brief Returns the benchmark name.
     * @return Name used in logs and output.
     */
    std::string_view name() const noexcept override;

    /**
     * @brief Runs the ROB benchmark.
     * @param data Output data. The function writes the ROB size here.
     */
    void measure(shared_types::CpuInfoData& data) override;

    /** @brief Fixes bad setting values before the run. */
    void validateConfig();

   private:
    Config config_;

    /** @brief Result for one filler count. */
    struct Result {
        size_t filler;               ///< Number of filler instructions.
        double min_cycles_per_iter;  ///< Best cycles per iteration.
        double avg_cycles_per_iter;  ///< Average cycles per iteration.
        double max_cycles_per_iter;  ///< Worst cycles per iteration.
    };

    /**
     * @brief Finds the first stable jump in the sweep.
     * @param results Measured points in order.
     * @return Estimated ROB size, or a negative value if no clear jump was found.
     */
    int detectRobSaturation(const std::vector<Result>& results);
};

}  // namespace silicon_probe::rob
