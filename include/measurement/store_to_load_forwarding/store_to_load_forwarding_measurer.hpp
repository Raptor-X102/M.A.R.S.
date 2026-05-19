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

/** @brief Result for one `(size, offset)` test. */
struct StoreToLoadForwardingResult {
    double                avg_ticks;   ///< Average cycles for one store+load pair.
    double                ticks_std;   ///< Std. dev. of the cycles.
    std::vector<uint64_t> avg_events;  ///< Average counter values for this test.
};

/**
 * @brief Measures store-to-load forwarding limits.
 *
 * It tests offsets for store sizes 8, 4, 2 and 1 bytes.
 * Then it finds the largest size and offset that still forward well.
 */
class StoreToLoadForwardingMeasurer final : public core::Measurer {
   public:
    static constexpr size_t kDefaultBufferSize       = 128;  // bytes
    static constexpr size_t kDefaultMinOffset        = 0;
    static constexpr size_t kDefaultMaxOffset        = 7;  // 0..7
    static constexpr size_t kDefaultOffsetStep       = 1;
    static constexpr size_t kDefaultIterations       = 100'000'000;
    static constexpr size_t kDefaultRepeats          = 10;
    static constexpr size_t kDefaultWarmupIterations = 10;

    /** @brief Settings for the store-to-load forwarding benchmark. */
    struct Config {
        bool   enabled                        = true;                     ///< Turn this measurer on or off.
        platform::MeasurementEnvironmentOptions environment;              ///< CPU and scheduler settings for the run.
        size_t min_offset                     = kDefaultMinOffset;        ///< Smallest offset to test.
        size_t max_offset                     = kDefaultMaxOffset;        ///< Largest offset to test.
        size_t offset_step                    = kDefaultOffsetStep;       ///< Step between offsets.
        size_t iterations                     = kDefaultIterations;       ///< Number of pairs inside one repeat.
        size_t repeats                        = kDefaultRepeats;          ///< Number of repeats for one point.
        size_t warmup_iterations              = kDefaultWarmupIterations; ///< Warm-up runs before timing.
        double time_growth_ratio              = 1.5;                      ///< Max time growth that still looks good.
        double pmc_saturation_ratio           = 0.01;                     ///< Max counter ratio that still looks good.
    };

    /** @brief Builds the measurer with default settings. */
    StoreToLoadForwardingMeasurer();

    /**
     * @brief Builds the measurer with custom settings.
     * @param config Settings for the next runs.
     */
    explicit StoreToLoadForwardingMeasurer(Config config);

    /**
     * @brief Returns the benchmark name.
     * @return Name used in logs and output.
     */
    std::string_view name() const noexcept override;

    /**
     * @brief Runs the forwarding benchmark.
     * @param data Output data. The function writes the best size and offset here.
     */
    void measure(shared_types::CpuInfoData& data) override;

   private:
    Config config_;

    /** @brief Fixes bad setting values before the run. */
    void validateConfig();

    /**
     * @brief Runs one store-size and offset test.
     * @tparam N Store size in bytes. Supported values are 1, 2, 4 and 8.
     * @param offset Load offset from the store address.
     * @param pmc Optional counter group for forwarding-fail events.
     * @param ev_names Event names in the same order as the counter values.
     * @return Result for this test.
     */
    template <size_t N>
    StoreToLoadForwardingResult
    run_test(size_t offset, platform::pmc::PmcGroup* pmc, const std::vector<std::string>& ev_names);
};

}  // namespace silicon_probe::store_to_load_forwarding
