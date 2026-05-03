// measurement/uops_cache/uops_cache_measurer.hpp
#pragma once

#include <algorithm>
#include <cmath>
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

struct IstructionData {
    platform::arch::InstrType instr_type;
    std::string instr_name;
};

struct UopsCacheResult {
    double avg_ticks_per_instr;
    double avg_ticks_per_iter;
    double ticks_std;
    std::vector<uint64_t> avg_events_counts;
};

struct UopsCacheSaturationPoint {
    size_t size_uops;
    double confidence;
    std::string reasoning;
};

class UopsCacheMeasurer final : public core::Measurer {
public:
    using InstrType = platform::arch::InstrType;

    static constexpr size_t kDefaultMinInstrCnt = 1200;
    static constexpr size_t kDefaultMaxInstrCnt = 5000;
    static constexpr size_t kDefaultInstrStep = 100;
    static constexpr size_t kDefaultIterations = 100'000;
    static constexpr size_t kDefaultRepeats = 10;
    static constexpr size_t kDefaultWarmupIterations = 100;

    struct Config {
        bool enabled = true;
        platform::MeasurementEnvironmentOptions environment;
        size_t min_instr_cnt = kDefaultMinInstrCnt;
        size_t max_instr_cnt = kDefaultMaxInstrCnt;
        size_t instr_step = kDefaultInstrStep;
        size_t iterations = kDefaultIterations;
        size_t repeats = kDefaultRepeats;
        size_t warmup_iterations = kDefaultWarmupIterations;
        IstructionData instr = {InstrType::ADD_REG, "add reg"};
        double dsb_share_stop = 0.3;
        double dsb_share_refine = 0.8;
        double dsb_drop_significant = 0.2;
        size_t coarse_ignore_first = 3;
    };

    UopsCacheMeasurer();
    explicit UopsCacheMeasurer(Config config);

    std::string_view name() const noexcept override;
    void measure(shared_types::CpuInfoData& data) override;

private:
    Config config_;

    UopsCacheResult run_test(size_t instr_cnt, platform::pmc::PmcGroup* pmc,
                             const std::vector<std::string>& uops_events);
    size_t findApproxSaturation(const std::vector<size_t>& counts, const std::vector<UopsCacheResult>& results,
                                const std::vector<std::string>& uops_events);
    size_t refineSaturation(size_t approx, platform::pmc::PmcGroup* pmc,
                            const std::vector<std::string>& uops_events);
};

}  // namespace silicon_probe::uops_cache
