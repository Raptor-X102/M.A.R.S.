#pragma once

#include <algorithm>
#include <cmath>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

#include "core/measurer.hpp"
#include "infra/logging.hpp"
#include "platform/arch.hpp"
#include "platform/events_discovery.hpp"
#include "platform/os.hpp"
#include "platform/pmc.hpp"

namespace silicon_probe::uops_cache {

/** @brief Instruction description for generated code. */
struct InstructionData {  
    platform::arch::InstrType instr_type; ///< Instruction type for the JIT generator.
    std::string              instr_name;  ///< Short name for logs and output.
};

/** @brief Result for one instruction count. */
struct UopsCacheResult {
    double                avg_ticks_per_instr; ///< Average cycles per instruction.
    double                ticks_std;           ///< Std. dev. of the cycles.
    std::vector<uint64_t> avg_events_counts;   ///< Average counter values for this point.
};

/** @brief Final uop-cache limit description. */
struct UopsCacheSaturationPoint {
    size_t      size_uops;   ///< Estimated size in uops.
    double      confidence;  ///< Confidence in the range [0, 1].
    std::string reasoning;   ///< Short reason for the result.
};

/**
 * @brief Measures uops cache size.
 *
 * It makes larger and larger instruction streams.
 * Then it checks when DSB share drops.
 */
class UopsCacheMeasurer final : public core::Measurer {
public:
    using InstrType = platform::arch::InstrType;

    static constexpr size_t kDefaultMinInstrCnt      = 1200;
    static constexpr size_t kDefaultMaxInstrCnt      = 5000;
    static constexpr size_t kDefaultInstrStep        = 100;
    static constexpr size_t kDefaultIterations       = 100'000;
    static constexpr size_t kDefaultRepeats          = 10;
    static constexpr size_t kDefaultWarmupIterations = 100;

    /** @brief Settings for the uops-cache benchmark. */
    struct Config {
        bool   enabled                        = true;                     ///< Turn this measurer on or off.
        platform::MeasurementEnvironmentOptions environment;              ///< CPU and scheduler settings for the run.
        size_t min_instr_cnt                  = kDefaultMinInstrCnt;     ///< Smallest instruction count to test.
        size_t max_instr_cnt                  = kDefaultMaxInstrCnt;     ///< Largest instruction count to test.
        size_t instr_step                     = kDefaultInstrStep;       ///< Step between instruction counts.
        size_t iterations                     = kDefaultIterations;      ///< Number of calls inside one repeat.
        size_t repeats                        = kDefaultRepeats;         ///< Number of repeats for one point.
        size_t warmup_iterations              = kDefaultWarmupIterations;///< Warm-up calls before timing.
        InstructionData instr                 = {InstrType::ADD_REG, "add reg"}; ///< Instruction used in generated code.
        double dsb_share_stop                 = 0.3;                     ///< DSB share that marks a clear drop.
        double dsb_share_refine               = 0.8;                     ///< DSB share that still looks good.
        double dsb_drop_significant           = 0.2;                     ///< Smallest DSB-share drop to trust.
        size_t coarse_ignore_first            = 3;                       ///< Number of first coarse points to ignore.
    };

    /** @brief Builds the measurer with default settings. */
    UopsCacheMeasurer();

    /**
     * @brief Builds the measurer with custom settings.
     * @param config Settings for the next runs.
     */
    explicit UopsCacheMeasurer(Config config);

    /**
     * @brief Returns the benchmark name.
     * @return Name used in logs and output.
     */
    std::string_view name() const noexcept override;

    /**
     * @brief Runs the uops-cache benchmark.
     * @param data Output data. The function writes the uops-cache size here.
     */
    void measure(shared_types::CpuInfoData& data) override;

private:
    Config config_;

    /** @brief Fixes bad setting values before the run. */
    void validateConfig();

    /**
     * @brief Returns the uop size of one instruction.
     * @param type Instruction type.
     * @return Number of uops for one instruction.
     */
    static uint8_t get_instr_uops_size(InstrType type);

    /**
     * @brief Measures one instruction-count point.
     * @param instr_cnt Number of generated instructions.
     * @param pmc Counter group for MITE and DSB events.
     * @param uops_events Event names in the same order as the counter values.
     * @return Result for this instruction count.
     */
    UopsCacheResult run_test(size_t instr_cnt,
                             platform::pmc::PmcGroup* pmc,
                             const std::vector<std::string>& uops_events);

    /**
     * @brief Finds a rough limit from the first scan.
     * @param counts Tested instruction counts in order.
     * @param results Measured results in the same order.
     * @return Rough limit in instructions, or zero if no clear drop was found.
     */
    size_t findApproxSaturation(const std::vector<size_t>& counts,
                                const std::vector<UopsCacheResult>& results);

    /**
     * @brief Refines the rough limit with a local binary search.
     * @param approx Rough limit from the first scan.
     * @param pmc Counter group for MITE and DSB events.
     * @param uops_events Event names in the same order as the counter values.
     * @return Refined limit in instructions.
     */
    size_t refineSaturation(size_t approx,
                            platform::pmc::PmcGroup* pmc,
                            const std::vector<std::string>& uops_events);
};

}  // namespace silicon_probe::uops_cache
