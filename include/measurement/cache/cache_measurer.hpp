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
#include "measurement/cache/boundary_analyzer.hpp"
#include "measurement/cache/cache_profiler_list.hpp"
#include "platform/arch.hpp"
#include "platform/events_discovery.hpp"
#include "platform/os.hpp"
#include "platform/pmc.hpp"
#include "shared_types/cache_types.hpp"

namespace silicon_probe::cache {

using CacheLevel = silicon_probe::shared_types::CacheLevel;

class CacheMeasurer final : public core::Measurer {
public:
    static constexpr size_t kL1MaxSize = 128 * 1024;
    static constexpr size_t kL2MaxSize = 2 * 1024 * 1024;
    static constexpr size_t kL3MaxSize = 128 * 1024 * 1024;
    static constexpr size_t kDefaultCacheMinLines = 16;
    static constexpr unsigned int kDefaultSeed = 0xBEAF;
    static constexpr size_t kDefaultWarmupIterations = 4;
    static constexpr size_t kDefaultPrecision = 64;
    static constexpr double kL1GrowthFactor = 1.42;
    static constexpr double kL2GrowthFactor = 1.8;
    static constexpr double kL3GrowthFactor = 6.0;
    static constexpr int kBaselineSamples = 3;
    static constexpr double kStabilityThreshold = 0.20;
    static constexpr size_t kDefaultTargetAccesses = 2'000'000;
    static constexpr size_t kDefaultMinIterations = 1;
    static constexpr size_t kDefaultMaxIterations = 1000;
    static constexpr double kDecisionTolerance = 0.10;
    static constexpr double kDefaultL2RefinementGrowthMultiplier = 1.15;
    static constexpr double kDefaultL1MissRateThreshold = 0.01;
    static constexpr double kDefaultL2MissRateThreshold = 0.25;
    static constexpr double kDefaultL3MissRateThreshold = 0.9;
    static constexpr double kDefaultL1MissGrowthFactor = 90.0;
    static constexpr double kDefaultL2MissGrowthFactor = 2.0;
    static constexpr double kDefaultL3MissGrowthFactor = 3.2;

    struct Config {
        bool enabled = true;
        std::bitset<3> levels = std::bitset<3>(0b111);
        size_t l1_max = kL1MaxSize;
        size_t l2_max = kL2MaxSize;
        size_t l3_max = kL3MaxSize;
        size_t cache_min_lines = kDefaultCacheMinLines;
        unsigned int seed = kDefaultSeed;
        size_t warmup_iterations = kDefaultWarmupIterations;
        size_t precision = kDefaultPrecision;
        size_t target_accesses = kDefaultTargetAccesses;
        size_t min_iterations = kDefaultMinIterations;
        size_t max_iterations = kDefaultMaxIterations;
        size_t refinement_samples = kBaselineSamples;
        double baseline_stability_threshold = kStabilityThreshold;
        double l1_growth_factor = kL1GrowthFactor;
        double l2_growth_factor = kL2GrowthFactor;
        double l3_growth_factor = kL3GrowthFactor;
        double l2_refinement_growth_multiplier = kDefaultL2RefinementGrowthMultiplier;
        double decision_tolerance = kDecisionTolerance;

        double l1_miss_rate_threshold = kDefaultL1MissRateThreshold;
        double l2_miss_rate_threshold = kDefaultL2MissRateThreshold;
        double l3_miss_rate_threshold = kDefaultL3MissRateThreshold;
        double l1_miss_growth_factor = kDefaultL1MissGrowthFactor;
        double l2_miss_growth_factor = kDefaultL2MissGrowthFactor;
        double l3_miss_growth_factor = kDefaultL3MissGrowthFactor;

        platform::MeasurementEnvironmentOptions environment;
    };

    CacheMeasurer();
    explicit CacheMeasurer(Config config);

    std::string_view name() const noexcept override;
    void measure(shared_types::CpuInfoData& data) override;

private:
    struct MeasurementResult {
        size_t size_bytes = 0;
        double cycles_per_element = 0.0;
        double miss_rate = 0.0;
        bool has_pmc = false;
    };

    struct BoundaryResult {
        size_t index = 0;
        double baseline_value = 0.0;
    };

    Config config_;
    size_t cache_line_size_ = 0;

    void measure_level(shared_types::CpuInfoData& data, CacheLevel level, size_t min_size, size_t max_size,
                       std::optional<size_t> shared_types::CpuInfoData::* target_field);
    std::unique_ptr<platform::pmc::PmcGroup> open_pmc_for_level(CacheLevel level,
                                                                shared_types::CpuInfoData& data) const;
    std::vector<MeasurementResult> measure_range(size_t min_size, size_t max_size,
                                                 std::unique_ptr<platform::pmc::PmcGroup>& pmc);
    MeasurementResult do_single_measurement_without_pmc(size_t size);
    MeasurementResult do_single_measurement_with_pmc(size_t size, platform::pmc::PmcGroup& pmc);
    BoundaryResult detect_latency_boundary(const std::vector<MeasurementResult>& results) const;
    size_t detect_miss_rate_boundary(const std::vector<MeasurementResult>& results, CacheLevel level) const;
    size_t refine_boundary_latency(const std::vector<MeasurementResult>& results, const BoundaryResult& boundary);
    size_t refine_boundary_misses(const std::vector<MeasurementResult>& results, size_t miss_index, CacheLevel level,
                                  std::unique_ptr<platform::pmc::PmcGroup>& pmc);
    void flush_cache_and_warmup(CacheProfilerList& list, size_t count) const;
    double growth_factor_for(size_t size_bytes) const noexcept;
    static size_t level_index(CacheLevel level) noexcept;
    static const char* level_name(CacheLevel level) noexcept;
};

}  // namespace silicon_probe::cache
