// measurement/store_to_load_forwarding/store_to_load_forwarding_measurer.hpp
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <string>
#include <vector>

#include "core/measurer.hpp"
#include "infra/logging.hpp"
#include "platform/arch.hpp"
#include "platform/events_discovery.hpp"
#include "platform/os.hpp"
#include "platform/pmc.hpp"

namespace silicon_probe::store_to_load_forwarding {

struct StoreToLoadForwardingResult {
    double avg_ticks;
    double ticks_std;
    std::vector<uint64_t> avg_events;
};

class StoreToLoadForwardingMeasurer final : public core::Measurer {
public:
    static constexpr size_t kDefaultBufferSize = 128;  // bytes
    static constexpr size_t kDefaultMinOffset = 0;
    static constexpr size_t kDefaultMaxOffset = 7;  // 0..7
    static constexpr size_t kDefaultOffsetStep = 1;
    static constexpr size_t kDefaultIterations = 100'000'000;
    static constexpr size_t kDefaultRepeats = 10;
    static constexpr size_t kDefaultWarmupIterations = 10;

    struct Config {
        bool enabled = true;
        platform::MeasurementEnvironmentOptions environment;
        size_t min_offset = kDefaultMinOffset;
        size_t max_offset = kDefaultMaxOffset;
        size_t offset_step = kDefaultOffsetStep;
        size_t iterations = kDefaultIterations;
        size_t repeats = kDefaultRepeats;
        size_t warmup_iterations = kDefaultWarmupIterations;
        double time_growth_ratio = 1.5;
        double pmc_saturation_ratio = 0.01;
    };

    StoreToLoadForwardingMeasurer();
    explicit StoreToLoadForwardingMeasurer(Config config);

    std::string_view name() const noexcept override;
    void measure(shared_types::CpuInfoData& data) override;

private:
    Config config_;

    template <size_t N>
    StoreToLoadForwardingResult run_test(size_t offset, platform::pmc::PmcGroup* pmc,
                                         const std::vector<std::string>& ev_names);
};

}  // namespace silicon_probe::store_to_load_forwarding
