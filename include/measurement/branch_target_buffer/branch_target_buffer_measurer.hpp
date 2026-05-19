// measurement/branch_target_buffer/branch_target_buffer_measurer.hpp
#pragma once

#include <algorithm>
#include <cmath>
#include <numeric>
#include <optional>
#include <string>
#include <vector>

#include "core/measurer.hpp"
#include "infra/logging.hpp"
#include "platform/arch.hpp"
#include "platform/events_discovery.hpp"
#include "platform/os.hpp"
#include "platform/pmc.hpp"

namespace silicon_probe::branch_target_buffer {

/** @brief Branch instruction description for generated code. */
struct InstructionData {
    platform::arch::InstrType instr_type;  ///< Instruction type for the JIT generator.
    std::string instr_name;                ///< Short name for logs and output.
};

/** @brief Result for one BTB test point. */
struct BranchTargetBufferResult {
    double avg_ticks_per_block;  ///< Average cycles per block.
    double ticks_std;            ///< Std. dev. of cycles per block.
    uint64_t avg_events_counts;  ///< Average BTB-related counter value.
};

/**
 * @brief Measures branch target buffer size.
 *
 * This benchmark makes branch chains with more and more blocks.
 * It looks for the point where time or misses go up.
 */
class BranchTargetBufferMeasurer final : public core::Measurer {
   public:
    static constexpr size_t kDefaultMinBlocksCnt     = 3500;
    static constexpr size_t kDefaultMaxBlocksCnt     = 5000;
    static constexpr size_t kDefaultBlocksStep       = 100;
    static constexpr size_t kDefaultIterations       = 100'000;
    static constexpr size_t kDefaultRepeats          = 10;
    static constexpr size_t kDefaultWarmupIterations = 100;
    static constexpr size_t kDefaultAlignment        = 16;

    /** @brief Settings for the BTB benchmark. */
    struct Config {
        bool enabled = true;                                               ///< Turn this measurer on or off.
        platform::MeasurementEnvironmentOptions environment;               ///< CPU and scheduler settings for the run.
        size_t min_blocks_cnt                     = kDefaultMinBlocksCnt;  ///< Smallest block count to test.
        size_t max_blocks_cnt                     = kDefaultMaxBlocksCnt;  ///< Largest block count to test.
        size_t blocks_step                        = kDefaultBlocksStep;    ///< Step between block counts.
        size_t iterations                         = kDefaultIterations;    ///< Loop count inside one generated test.
        size_t repeats                            = kDefaultRepeats;       ///< Number of repeats for one point.
        size_t warmup_iterations                  = kDefaultWarmupIterations;  ///< Warm-up runs before timing.
        int alignment                             = kDefaultAlignment;         ///< Block alignment in generated code.
        double misprediction_saturation_threshold = 0.01;   ///< Miss-rate limit for a clear overflow.
        double misprediction_growth_threshold     = 0.005;  ///< Smallest miss-rate growth to treat as a jump.
        double time_growth_ratio                  = 1.20;   ///< Time growth ratio for time-only mode.
        size_t time_stability_points              = 3;      ///< Number of points used to check stable growth.
        size_t coarse_ignore_first                = 2;      ///< Number of first coarse points to ignore.
    };

    /** @brief Builds the measurer with default settings. */
    BranchTargetBufferMeasurer();

    /**
     * @brief Builds the measurer with custom settings.
     * @param config Settings for the next runs.
     */
    explicit BranchTargetBufferMeasurer(Config config);

    /**
     * @brief Returns the benchmark name.
     * @return Name used in logs and output.
     */
    std::string_view name() const noexcept override;

    /**
     * @brief Runs the BTB benchmark.
     * @param data Output data. The function writes the BTB size here.
     */
    void measure(shared_types::CpuInfoData& data) override;

   private:
    Config config_;

    /** @brief Fixes bad setting values before the run. */
    void validateConfig();

    /**
     * @brief Measures one branch chain.
     * @param blocks_cnt Number of branch blocks in generated code.
     * @param pmc Optional counter group for BTB events.
     * @return Result for this block count.
     */
    BranchTargetBufferResult run_test(size_t blocks_cnt, platform::pmc::PmcGroup* pmc);

    /**
     * @brief Converts counter data to a miss rate.
     * @param res Measured result for one block count.
     * @param blocks_cnt Number of branch blocks in this result.
     * @return Average misses per branch.
     */
    double computeMispredictionRate(const BranchTargetBufferResult& res, size_t blocks_cnt) const;

    /**
     * @brief Finds a rough BTB size from the first scan.
     * @param counts Tested block counts in order.
     * @param results Measured results in the same order.
     * @param use_events `true` to use counter data first.
     * @return Rough BTB size, or `std::nullopt` if no jump was found.
     */
    std::optional<size_t> findApproxSaturation(
        const std::vector<size_t>& counts, const std::vector<BranchTargetBufferResult>& results, bool use_events
    );

    /**
     * @brief Refines a counter-based BTB limit.
     * @param good Largest good block count.
     * @param bad Smallest bad block count.
     * @param pmc Counter group for BTB events.
     * @return Refined BTB size in entries.
     */
    size_t refineSaturation(size_t good, size_t bad, platform::pmc::PmcGroup* pmc);

    /**
     * @brief Refines a time-based BTB limit.
     * @param approx Rough BTB size from the first scan.
     * @param baseline Base time before the jump.
     * @param pmc Optional counter group. Not used in time-only mode.
     * @return Refined BTB size in entries.
     */
    size_t refineSaturationTime(size_t approx, double baseline, platform::pmc::PmcGroup* pmc);
};

}  // namespace silicon_probe::branch_target_buffer
