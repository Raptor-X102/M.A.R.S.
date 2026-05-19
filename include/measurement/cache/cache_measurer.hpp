// measurement/cache/cache_measurer.hpp
#pragma once

#include <algorithm>
#include <bitset>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "core/measurer.hpp"
#include "infra/logging.hpp"
#include "measurement/cache/cache_profiler_list.hpp"
#include "measurement/common/statistics.hpp"
#include "platform/arch.hpp"
#include "platform/events_discovery.hpp"
#include "platform/os.hpp"
#include "platform/pmc.hpp"
#include "shared_types/cache_types.hpp"

namespace silicon_probe::cache {

using CacheLevel = silicon_probe::shared_types::CacheLevel;
namespace statistics = silicon_probe::common::statistics;

/**
 * @brief Measures cache size.
 *
 * This benchmark uses pointer lists with a growing size.
 * It looks for a jump in time or miss rate.
 */
class CacheMeasurer final : public core::Measurer {
   public:
    static constexpr size_t kL1MaxSize                           = 128 * 1024;
    static constexpr size_t kL2MaxSize                           = 2 * 1024 * 1024;
    static constexpr size_t kL3MaxSize                           = 64 * 1024 * 1024;
    static constexpr size_t kDefaultCacheMinLines                = 16;
    static constexpr unsigned int kDefaultSeed                   = 0xBEAF;
    static constexpr size_t kDefaultWarmupIterations             = 4;
    static constexpr size_t kDefaultPrecision                    = 64;
    static constexpr double kL1GrowthFactor                      = 1.42;
    static constexpr double kL2GrowthFactor                      = 1.8;
    static constexpr double kL3GrowthFactor                      = 3.0;
    static constexpr int kBaselineSamples                        = 3;
    static constexpr double kStabilityThreshold                  = 0.20;
    static constexpr size_t kDefaultTargetAccesses               = 2'000'000;
    static constexpr size_t kDefaultMinIterations                = 1;
    static constexpr size_t kDefaultMaxIterations                = 1000;
    static constexpr double kDecisionTolerance                   = 0.10;
    static constexpr double kDefaultL2RefinementGrowthMultiplier = 1.15;
    static constexpr double kDefaultL1MissRateThreshold          = 0.01;
    static constexpr double kDefaultL2MissRateThreshold          = 0.25;
    static constexpr double kDefaultL3MissRateThreshold          = 0.25;
    static constexpr double kDefaultL1MissGrowthFactor           = 90.0;
    static constexpr double kDefaultL2MissGrowthFactor           = 2.0;
    static constexpr double kDefaultL3MissGrowthFactor           = 50;

    /** @brief Settings for the cache benchmark. */
    struct Config {
        bool                               enabled                           = true;               ///< Turn this measurer on or off.
        std::bitset<3>                     levels                            = std::bitset<3>(0b111); ///< Cache levels to test.
        size_t                             l1_max                            = kL1MaxSize;        ///< Largest L1 test size in bytes.
        size_t                             l2_max                            = kL2MaxSize;        ///< Largest L2 test size in bytes.
        size_t                             l3_max                            = kL3MaxSize;        ///< Largest L3 test size in bytes.
        size_t                             cache_min_lines                   = kDefaultCacheMinLines; ///< Smallest test size in cache lines.
        bool                               use_huge_pages                    = false;             ///< Use huge pages for the test buffer.
        unsigned int                       seed                              = kDefaultSeed;      ///< Seed for list generation.
        size_t                             warmup_iterations                 = kDefaultWarmupIterations; ///< Warm-up runs before timing.
        size_t                             precision                         = kDefaultPrecision; ///< Target precision in bytes.
        size_t                             target_accesses                   = kDefaultTargetAccesses; ///< Target number of accesses in one sample.
        size_t                             min_iterations                    = kDefaultMinIterations; ///< Smallest loop count for one sample.
        size_t                             max_iterations                    = kDefaultMaxIterations; ///< Largest loop count for one sample.
        size_t                             refinement_samples                = kBaselineSamples;  ///< Number of samples in refine mode.
        double                             baseline_stability_threshold      = kStabilityThreshold; ///< Allowed base noise.
        double                             l1_growth_factor                  = kL1GrowthFactor;   ///< Time growth ratio for L1.
        double                             l2_growth_factor                  = kL2GrowthFactor;   ///< Time growth ratio for L2.
        double                             l3_growth_factor                  = kL3GrowthFactor;   ///< Time growth ratio for L3.
        double                             l2_refinement_growth_multiplier   = kDefaultL2RefinementGrowthMultiplier; ///< Extra L2 refine factor.
        double                             decision_tolerance                = kDecisionTolerance; ///< Max gap between two final estimates.

        double                             l1_miss_rate_threshold            = kDefaultL1MissRateThreshold; ///< Miss-rate limit for L1.
        double                             l2_miss_rate_threshold            = kDefaultL2MissRateThreshold; ///< Miss-rate limit for L2.
        double                             l3_miss_rate_threshold            = kDefaultL3MissRateThreshold; ///< Miss-rate limit for L3.
        double                             l1_miss_growth_factor             = kDefaultL1MissGrowthFactor;  ///< Miss growth ratio for L1.
        double                             l2_miss_growth_factor             = kDefaultL2MissGrowthFactor;  ///< Miss growth ratio for L2.
        double                             l3_miss_growth_factor             = kDefaultL3MissGrowthFactor;  ///< Miss growth ratio for L3.

        platform::MeasurementEnvironmentOptions environment;                 ///< CPU and scheduler settings for the run.
    };

    /** @brief Result for one cache test point. */
    struct MeasurementResult {
        size_t size_bytes         = 0;     ///< Tested size in bytes.
        double cycles_per_element = 0.0;   ///< Average cycles per element.
        double miss_rate          = 0.0;   ///< Measured miss rate.
        bool   has_pmc            = false; ///< `true` if counter data is valid.
    };

    /** @brief Builds the measurer with default settings. */
    CacheMeasurer();

    /**
     * @brief Builds the measurer with custom settings.
     * @param config Settings for the next runs.
     */
    explicit CacheMeasurer(Config config);

    /**
     * @brief Returns the benchmark name.
     * @return Name used in logs and output.
     */
    std::string_view name() const noexcept override;

    /**
     * @brief Runs the cache benchmark.
     * @param data Output data. The function writes cache sizes here.
     */
    void measure(shared_types::CpuInfoData& data) override;

   private:
    /** @brief Rough jump found in the first scan. */
    struct BoundaryResult {
        size_t index          = 0;   ///< Index of the first jump candidate.
        double baseline_value = 0.0; ///< Base time near this candidate.
    };

    Config config_;
    size_t cache_line_size_ = 0;
    std::unique_ptr<CacheProfilerList> reusable_list_;
    size_t reusable_max_size_ = 0;

    /** @brief Fixes bad setting values before the run. */
    void validateConfig();

    /**
     * @brief Measures one cache level.
     * @param data Output data for the final size.
     * @param level Cache level to test.
     * @param min_size Smallest size in bytes.
     * @param max_size Largest size in bytes.
     * @param target_field Field in @p data that gets the result.
     */
    void measure_level(shared_types::CpuInfoData& data,
                       CacheLevel level,
                       size_t min_size,
                       size_t max_size,
                       std::optional<size_t> shared_types::CpuInfoData::* target_field);

    /**
     * @brief Opens miss counters for one cache level.
     * @param level Cache level to test.
     * @param data Known CPU data.
     * @return Counter group, or `nullptr` for time-only mode.
     */
    std::unique_ptr<platform::pmc::PmcGroup> open_pmc_for_level(CacheLevel level,
                                                                shared_types::CpuInfoData& data) const;

    /**
     * @brief Measures many sizes in one range.
     * @param min_size Smallest size in bytes.
     * @param max_size Largest size in bytes.
     * @param pmc Optional counter group for miss data.
     * @return Measured points in order.
     */
    std::vector<MeasurementResult> measure_range(size_t min_size,
                                                 size_t max_size,
                                                 std::unique_ptr<platform::pmc::PmcGroup>& pmc);

    /**
     * @brief Measures one size without counters.
     * @param list Pointer list for the test.
     * @param count Number of active elements.
     * @return Time-only result.
     */
    MeasurementResult do_single_measurement_without_pmc(CacheProfilerList* list, size_t count);

    /**
     * @brief Measures one size with counters.
     * @param list Pointer list for the test.
     * @param count Number of active elements.
     * @param pmc Counter group for cache misses.
     * @return Result with time and miss data.
     */
    MeasurementResult do_single_measurement_with_pmc(CacheProfilerList* list, size_t count, 
                                                     platform::pmc::PmcGroup& pmc);

    /**
     * @brief Finds a rough time jump.
     * @param results Measured points for one cache level.
     * @return Rough jump index and base time.
     */
    BoundaryResult detect_latency_boundary(const std::vector<MeasurementResult>& results) const;

    /**
     * @brief Finds a rough miss-rate jump.
     * @param results Measured points for one cache level.
     * @param level Cache level to test.
     * @return Index of the first bad point, or zero if none was found.
     */
    size_t detect_miss_rate_boundary(const std::vector<MeasurementResult>& results, CacheLevel level) const;

    /**
     * @brief Refines a rough time-based limit.
     * @param results Rough points for this cache level.
     * @param boundary Rough jump from the first scan.
     * @return Refined cache size in bytes.
     */
    size_t refine_boundary_latency(const std::vector<MeasurementResult>& results, const BoundaryResult& boundary);

    /**
     * @brief Refines a rough miss-based limit.
     * @param results Rough points for this cache level.
     * @param miss_index First bad point from miss data.
     * @param level Cache level to test.
     * @param pmc Counter group for miss data.
     * @return Refined cache size in bytes.
     */
    size_t refine_boundary_misses(const std::vector<MeasurementResult>& results,
                                  size_t miss_index,
                                  CacheLevel level,
                                  std::unique_ptr<platform::pmc::PmcGroup>& pmc);

    /**
     * @brief Flushes and warms the test buffer.
     * @param list Pointer list to prepare.
     * @param count Number of active elements.
     */
    void flush_cache_and_warmup(CacheProfilerList& list, size_t count) const;

    /**
     * @brief Returns the time growth factor for one size.
     * @param size_bytes Tested size in bytes.
     * @return Time growth ratio for this size.
     */
    double growth_factor_for(size_t size_bytes) const noexcept;

    /**
     * @brief Converts a cache level to a bitset index.
     * @param level Cache level.
     * @return Bitset index.
     */
    static size_t level_index(CacheLevel level) noexcept;

    /**
     * @brief Returns a short cache-level name.
     * @param level Cache level.
     * @return Static name for logs.
     */
    static const char* level_name(CacheLevel level) noexcept;

    /**
     * @brief Common code for one timed sample.
     * @tparam PreFn Function called before timing.
     * @tparam PostFn Function called after timing.
     * @param list Pointer list for the test.
     * @param count Number of active elements.
     * @param pre Setup hook before timing.
     * @param post Finish hook after timing.
     * @return Result for the current test size.
     */
    template <typename PreFn, typename PostFn>
    MeasurementResult
    measure_impl(CacheProfilerList* list, size_t count, PreFn&& pre, PostFn&& post) {
        // Pick loop count from the target access count.
        size_t iterations = config_.target_accesses / count;
        iterations = std::max(iterations, config_.min_iterations);
        iterations = std::min(iterations, config_.max_iterations);
        const uint64_t total_loads = static_cast<uint64_t>(count) * iterations;

        // Flush and warm the buffer before timing.
        flush_cache_and_warmup(*list, count);

        volatile CacheProfilerList::Element* element = list->first();

        pre();

        const uint64_t start = platform::arch::tick();
        for (size_t iter = 0; iter < iterations; ++iter) {
            for (size_t idx = 0; idx < count; ++idx) {
                element = element->next;
            }
        }
        const uint64_t end = platform::arch::tick();

        std::optional<double> miss_rate_opt = post(total_loads);

        MeasurementResult result;
        result.size_bytes = 0; // The caller sets the real size.
        result.cycles_per_element = static_cast<double>(end - start) / static_cast<double>(total_loads);
        result.has_pmc = miss_rate_opt.has_value();
        result.miss_rate = miss_rate_opt.value_or(0.0);
        return result;
    }

    /**
     * @brief Common binary search for the final limit.
     * @tparam MeasureFn Function that measures one midpoint.
     * @param left Left limit in bytes.
     * @param right Right limit in bytes.
     * @param precision Stop width in bytes.
     * @param growth_factor Ratio that marks a limit.
     * @param measure Function that returns one metric value.
     * @param baseline_mean Base value for comparison.
     * @return Refined limit in bytes.
     */
    template <typename MeasureFn>
    size_t
    refine_boundary(size_t left, size_t right, size_t precision, double growth_factor, MeasureFn&& measure, double baseline_mean) const {
        SPDLOG_INFO("[boundary] baseline={}, threshold={}x", baseline_mean, growth_factor);

        size_t current_left  = left;
        size_t current_right = right;

        while (current_right - current_left > precision) {
            const size_t midpoint = current_left + (current_right - current_left) / 2;

            std::vector<double> samples;
            samples.reserve(std::max<size_t>(1, config_.refinement_samples));
            for (size_t index = 0; index < config_.refinement_samples; ++index) {
                samples.push_back(measure(midpoint));
            }

            const auto statistics   = statistics::compute_stats(samples);
            const double ratio      = baseline_mean > 0.0 ? statistics.mean / baseline_mean : 0.0;
            const bool out_of_cache = ratio > growth_factor;

            SPDLOG_INFO(
                "[boundary] size={}, mean={}, ratio={}, threshold={}, decision={}",
                midpoint,
                statistics.mean,
                ratio,
                growth_factor,
                out_of_cache ? "out" : "in"
            );

            if (out_of_cache) {
                current_right = midpoint;
            } else {
                current_left = midpoint;
            }
        }

        const size_t boundary = (current_left + current_right) / 2;
        SPDLOG_INFO("[boundary] final={} bytes", boundary);
        return boundary;
    }
};

}  // namespace silicon_probe::cache
