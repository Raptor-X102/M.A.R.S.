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

struct InstructionData {
    platform::arch::InstrType instr_type;
    std::string instr_name;
};

struct BranchTargetBufferResult {
    double avg_ticks_per_block;
    double avg_ticks_per_iter;
    double ticks_std;
    uint64_t avg_events_counts;
};

class BranchTargetBufferMeasurer final : public core::Measurer {
public:
    static constexpr size_t kDefaultMinBlocksCnt = 3500;
    static constexpr size_t kDefaultMaxBlocksCnt = 5000;
    static constexpr size_t kDefaultBlocksStep = 100;
    static constexpr size_t kDefaultIterations = 100'000;
    static constexpr size_t kDefaultRepeats = 10;
    static constexpr size_t kDefaultWarmupIterations = 100;
    static constexpr size_t kDefaultAlignment = 16;

    struct Config {
        bool enabled = true;
        platform::MeasurementEnvironmentOptions environment;
        size_t min_blocks_cnt = kDefaultMinBlocksCnt;
        size_t max_blocks_cnt = kDefaultMaxBlocksCnt;
        size_t blocks_step = kDefaultBlocksStep;
        size_t iterations = kDefaultIterations;
        size_t repeats = kDefaultRepeats;
        size_t warmup_iterations = kDefaultWarmupIterations;
        int alignment = kDefaultAlignment;
        double misprediction_saturation_threshold = 0.01;
        double misprediction_growth_threshold = 0.005;
        double time_growth_ratio = 1.20;
        size_t time_stability_points = 3;
        size_t coarse_ignore_first = 2;
    };

    BranchTargetBufferMeasurer();
    explicit BranchTargetBufferMeasurer(Config config);

    std::string_view name() const noexcept override;
    void measure(shared_types::CpuInfoData& data) override;

private:
    Config config_;

    BranchTargetBufferResult run_test(size_t blocks_cnt, platform::pmc::PmcGroup* pmc);
    double computeMispredictionRate(const BranchTargetBufferResult& res, size_t blocks_cnt) const;
    size_t findApproxSaturation(const std::vector<size_t>& counts,
                                const std::vector<BranchTargetBufferResult>& results,
                                bool use_events);
    size_t refineSaturation(size_t good, size_t bad, platform::pmc::PmcGroup* pmc);
    size_t refineSaturationTime(size_t approx, platform::pmc::PmcGroup* pmc);
};

}  // namespace silicon_probe::branch_target_buffer
