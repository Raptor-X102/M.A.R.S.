// measurement/write_buffer/write_buffer_measurer.hpp
#pragma once

#include <cmath>
#include <cstring>
#include <memory>
#include <numeric>

#include "core/measurer.hpp"
#include "infra/logging.hpp"
#include "platform/arch.hpp"
#include "platform/events_discovery.hpp"
#include "platform/os.hpp"
#include "platform/pmc.hpp"

namespace silicon_probe::write_buffer {

/** @brief Result for one write count. */
struct WriteBufferResult {
    double                avg_latency_ticks; ///< Average time of the final store+load pair.
    double                latency_stddev;    ///< Std. dev. of the measured time.
    std::vector<uint64_t> avg_events;        ///< Average counter values for this point.
};

/**
 * @brief Measures write buffer size.
 *
 * This benchmark does more and more stores before one final store+load pair.
 * It looks for the point where time or stalls go up.
 */
class WriteBufferMeasurer final : public core::Measurer {
   public:
    static constexpr size_t kDefaultMaxWrites        = 64;
    static constexpr size_t kDefaultMinWrites        = 1;
    static constexpr size_t kDefaultWritesStep       = 1;
    static constexpr size_t kDefaultIterations       = 5000;
    static constexpr size_t kDefaultRepeats          = 30;
    static constexpr size_t kDefaultWarmupIterations = 5;
    static constexpr size_t kBufferSizeMB            = 16;
    static constexpr size_t kBytesPerEntry           = 4;
    static constexpr size_t kCacheLineSize           = 64;
    static constexpr size_t kStride = kCacheLineSize / kBytesPerEntry;   // = 16

    /** @brief Settings for the write-buffer benchmark. */
    struct Config {
        bool   enabled                        = true;                     ///< Turn this measurer on or off.
        platform::MeasurementEnvironmentOptions environment;              ///< CPU and scheduler settings for the run.
        size_t max_writes                     = kDefaultMaxWrites;        ///< Largest write count to test.
        size_t min_writes                     = kDefaultMinWrites;        ///< Smallest write count to test.
        size_t writes_step                    = kDefaultWritesStep;       ///< Step between write counts.
        size_t iterations                     = kDefaultIterations;       ///< Number of samples inside one repeat.
        size_t repeats                        = kDefaultRepeats;          ///< Number of repeats for one point.
        size_t warmup_iterations              = kDefaultWarmupIterations; ///< Warm-up runs before timing.

        double latency_spike_ratio            = 2.0;                      ///< Time ratio that marks a clear jump.
        double latency_hold_ratio             = 1.5;                      ///< Time ratio to confirm the jump.
        double stall_fallback_ratio           = 0.9;                      ///< Stall ratio for fallback logic.
        size_t baseline_window                = 3;                        ///< Number of first points for the base time.
        double stall_baseline_ratio           = 10.0;                     ///< Stall growth ratio over the base level.
        double stall_absolute_min             = 100.0;                    ///< Smallest stall value to trust.
        double stall_gradient_ratio           = 10.0;                     ///< Smallest stall jump between two points.
        size_t stall_median_window            = 3;                        ///< Window size for stall smoothing.
    };

    /** @brief Builds the measurer with default settings. */
    WriteBufferMeasurer();

    /**
     * @brief Builds the measurer with custom settings.
     * @param config Settings for the next runs.
     */
    explicit WriteBufferMeasurer(Config config);

    /**
     * @brief Returns the benchmark name.
     * @return Name used in logs and output.
     */
    std::string_view name() const noexcept override;

    /**
     * @brief Runs the write-buffer benchmark.
     * @param data Output data. The function writes the write-buffer size here.
     */
    void measure(shared_types::CpuInfoData& data) override;

   private:
    Config config_;

    /** @brief Fixes bad setting values before the run. */
    void validateConfig();

    /**
     * @brief Measures one write-count point.
     * @param num_writes Number of stores before the final pair.
     * @param fill_base Base address for the fill area.
     * @param extra_addr Address for the final store+load pair.
     * @param dummy Volatile sink for the timed load.
     * @param pmc Optional counter group for store-buffer events.
     * @return Result for this write count.
     */
    WriteBufferResult measure_for_writes(
        size_t num_writes,
        int* fill_base,
        volatile int* extra_addr,
        volatile int& dummy,
        platform::pmc::PmcGroup* pmc
    );

    /**
     * @brief Finds the final write-buffer size from the sweep.
     * @param results Measured results in order.
     * @param writes_list Write counts in the same order.
     * @param sb_idx Index of the `resource_stalls.sb` event.
     * @param bound_idx Index of the `exe_activity.bound_on_stores` event.
     * @return Estimated write-buffer size in 4-byte entries.
     */
    size_t analyze_buffer_capacity(
        const std::vector<WriteBufferResult>& results,
        const std::vector<size_t>& writes_list,
        size_t sb_idx,
        size_t bound_idx
    ) const;
};

}  // namespace silicon_probe::write_buffer
