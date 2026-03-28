#pragma once

#include "silicon_probe/cache/cache_profiler_list.hpp"
#include "silicon_probe/core/measurer.hpp"
#include "silicon_probe/platform/os.hpp"

#include <bitset>
#include <cstddef>
#include <optional>
#include <vector>

namespace silicon_probe::cache {

enum class CacheLevel {
    l1d = 0,
    l2 = 1,
    l3 = 2,
};

class CacheMeasurer final : public core::Measurer {
public:
    static constexpr size_t kL1MaxSize = 128 * 1024;
    static constexpr size_t kL2MaxSize = 2 * 1024 * 1024;
    static constexpr size_t kL3MaxSize = 128 * 1024 * 1024;
    static constexpr size_t kDefaultCacheMinLines = 16;
    static constexpr unsigned int kDefaultSeed = 0xBEAF;
    static constexpr size_t kDefaultWarmupIterations = 4;
    static constexpr size_t kDefaultPrecision = 64;
    static constexpr double kL1GrowthFactor = 1.5;
    static constexpr double kL2GrowthFactor = 2.0;
    static constexpr double kL3GrowthFactor = 2.2;
    static constexpr int kBaselineSamples = 3;
    static constexpr double kStabilityThreshold = 0.2;
    static constexpr size_t kDefaultTargetAccesses = 2'000'000;
    static constexpr size_t kDefaultMinIterations = 1;
    static constexpr size_t kDefaultMaxIterations = 1000;

    struct Config {
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
        platform::MeasurementEnvironmentOptions environment;
    };

    CacheMeasurer();
    explicit CacheMeasurer(Config config);

    std::string_view name() const noexcept override;
    void measure(core::CpuInfoData& data) override;

private:
    struct MeasurementResult {
        size_t size_bytes = 0;
        double cycles_per_element = 0.0;
    };

    struct BoundaryResult {
        size_t index = 0;
        double baseline_latency = 0.0;
    };

    void measure_level(core::CpuInfoData& data,
                       CacheLevel level,
                       size_t min_size,
                       size_t max_size,
                       std::optional<size_t> core::CpuInfoData::* target_field);
    std::vector<MeasurementResult> measure_range(size_t min_size, size_t max_size);
    MeasurementResult do_single_measurement(size_t size);
    BoundaryResult detect_boundary(const std::vector<MeasurementResult>& results) const;
    size_t refine_boundary(const std::vector<MeasurementResult>& results, const BoundaryResult& boundary);
    void flush_cache_and_warmup(CacheProfilerList& list, size_t count) const;

    static size_t level_index(CacheLevel level) noexcept;
    static const char* level_name(CacheLevel level) noexcept;
    double growth_factor_for(size_t size_bytes) const noexcept;

    Config config_;
    size_t cache_line_size_ = 0;
};

} // namespace silicon_probe::cache
