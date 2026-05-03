// measurement/rob/rob_measurer.hpp
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

class RobMeasurer final : public core::Measurer {
public:
    static constexpr size_t kDefaultMinInstrCnt = 50;
    static constexpr size_t kDefaultMaxInstrCnt = 600;
    static constexpr size_t kDefaultInstrCntStep = 10;
    static constexpr size_t kDefaultWarmupIterations = 2;
    static constexpr size_t kDefaultInnerIterations = 8192;
    static constexpr size_t kDefaultOuterIterations = 64;
    static constexpr size_t kDefaultUnroll = 17;
    static constexpr int kDefaultInstrType = 4;

    struct Config {
        bool enabled = true;
        platform::MeasurementEnvironmentOptions environment;
        size_t min_instr_cnt = kDefaultMinInstrCnt;
        size_t max_instr_cnt = kDefaultMaxInstrCnt;
        size_t instr_cnt_step = kDefaultInstrCntStep;
        size_t warmup_iterations = kDefaultWarmupIterations;
        size_t inner_iterations = kDefaultInnerIterations;
        size_t outer_iterations = kDefaultOuterIterations;
        int instr_type = kDefaultInstrType;
        double baseline_fraction = 0.2;
        size_t baseline_min_samples = 5;
        size_t required_consecutive_points = 3;
        double saturation_threshold_ratio = 1.15;
        double fallback_jump_ratio = 0.5;
    };

    RobMeasurer();
    explicit RobMeasurer(Config config);

    std::string_view name() const noexcept override;
    void measure(shared_types::CpuInfoData& data) override;

private:
    Config config_;

    struct Result {
        size_t filler;
        double min_cycles_per_iter;
        double avg_cycles_per_iter;
        double max_cycles_per_iter;
    };

    int detectRobSaturation(const std::vector<Result>& results);
};

}  // namespace silicon_probe::rob
