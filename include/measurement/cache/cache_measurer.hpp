#pragma once

#include "measurement/cache/boundary_analyzer.hpp"
#include "measurement/cache/cache_profiler_list.hpp"
#include "infra/logging.hpp"
#include "measurement/core/measurer.hpp"
#include "platform/arch.hpp"
#include "platform/os.hpp"

#include <algorithm>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <utility>
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
        double l2_refinement_growth_multiplier = 1.5;
        platform::MeasurementEnvironmentOptions environment;
    };

  private:
    struct MeasurementResult {
        size_t size_bytes = 0;
        double cycles_per_element = 0.0;
    };

    struct BoundaryResult {
        size_t index = 0;
        double baseline_latency = 0.0;
    };

    Config config_;
    size_t cache_line_size_ = 0;

  public:
    CacheMeasurer() : CacheMeasurer(Config{}) {}

    explicit CacheMeasurer(Config config) : config_(std::move(config)), cache_line_size_(platform::cache_line_size()) {
        SPDLOG_INFO("[{}] configured with levels: L1={}, L2={}, L3={}", name(), config_.levels.test(level_index(CacheLevel::l1d)),
                    config_.levels.test(level_index(CacheLevel::l2)), config_.levels.test(level_index(CacheLevel::l3)));
    }

    std::string_view name() const noexcept override { return "cache"; }

    void measure(core::CpuInfoData& data) override {
        SPDLOG_INFO("[{}] starting cache measurement", name());
        platform::ScopedMeasurementEnvironment environment{config_.environment};

        data.cache_line_size = cache_line_size_;

        const size_t l1_min = config_.cache_min_lines * cache_line_size_;
        if (config_.levels.test(level_index(CacheLevel::l1d))) {
            measure_level(data, CacheLevel::l1d, l1_min, config_.l1_max, &core::CpuInfoData::l1d_size);
        }
        if (config_.levels.test(level_index(CacheLevel::l2))) {
            measure_level(data, CacheLevel::l2, config_.l1_max, config_.l2_max, &core::CpuInfoData::l2_size);
        }
        if (config_.levels.test(level_index(CacheLevel::l3))) {
            measure_level(data, CacheLevel::l3, config_.l2_max, config_.l3_max, &core::CpuInfoData::l3_size);
        }

        SPDLOG_INFO("[{}] cache measurement complete", name());
    }

  private:
    void measure_level(core::CpuInfoData& data, CacheLevel level, size_t min_size, size_t max_size,
                       std::optional<size_t> core::CpuInfoData::* target_field) {
        SPDLOG_INFO("=== Measuring {} ===", level_name(level));

        if (min_size == 0 || min_size > max_size) {
            SPDLOG_WARN("Skipping invalid range for {}: min={}, max={}", level_name(level), min_size, max_size);
            return;
        }

        const auto results = measure_range(min_size, max_size);
        if (results.empty()) {
            SPDLOG_WARN("No measurements collected for {}", level_name(level));
            return;
        }

        const auto boundary = detect_boundary(results);
        auto& target = data.*target_field;

        if (boundary.index > 0 && boundary.index < results.size()) {
            target = refine_boundary(results, boundary);
            data.cache_line_size = cache_line_size_;
            SPDLOG_INFO("{} size detected: {} bytes", level_name(level), *target);
        } else if (boundary.index == 0) {
            target = results.front().size_bytes;
            SPDLOG_WARN("{} may be smaller than {} bytes", level_name(level), *target);
        } else {
            SPDLOG_WARN("{} size detection failed", level_name(level));
        }
    }

    std::vector<MeasurementResult> measure_range(size_t min_size, size_t max_size) {
        std::vector<MeasurementResult> results{};
        for (size_t size = min_size; size <= max_size; size *= 2) {
            results.push_back(do_single_measurement(size));
            SPDLOG_INFO("Size={}, cycles/elem={}", results.back().size_bytes, results.back().cycles_per_element);

            if (size > max_size / 2) {
                break;
            }
        }
        return results;
    }

    MeasurementResult do_single_measurement(size_t size) {
        const size_t count = size / cache_line_size_;
        if (count == 0) {
            throw std::invalid_argument("Measurement size must be at least one cache line");
        }

        size_t iterations = config_.target_accesses / count;
        iterations = std::max(iterations, config_.min_iterations);
        iterations = std::min(iterations, config_.max_iterations);

        CacheProfilerList list{cache_line_size_, count, config_.seed};
        flush_cache_and_warmup(list, count);

        volatile CacheProfilerList::Element* element = list.first();
        const uint64_t start = platform::arch::tick();
        for (size_t iteration = 0; iteration < iterations; ++iteration) {
            for (size_t index = 0; index < count; ++index) {
                element = element->next;
            }
        }
        const uint64_t end = platform::arch::tick();

        return MeasurementResult{size, static_cast<double>(end - start) / static_cast<double>(count * iterations)};
    }

    BoundaryResult detect_boundary(const std::vector<MeasurementResult>& results) const {
        BoundaryResult boundary{};
        boundary.index = results.size();

        if (results.size() < 3) {
            return boundary;
        }

        double baseline_sum = 0.0;
        for (size_t index = 0; index < 3; ++index) {
            baseline_sum += results[index].cycles_per_element;
        }
        boundary.baseline_latency = baseline_sum / 3.0;

        for (size_t index = 1; index < results.size(); ++index) {
            const double ratio = results[index].cycles_per_element / boundary.baseline_latency;
            if (ratio >= growth_factor_for(results[index].size_bytes)) {
                boundary.index = index;
                break;
            }
        }

        return boundary;
    }

    size_t refine_boundary(const std::vector<MeasurementResult>& results, const BoundaryResult& boundary) {
        const size_t left = results[boundary.index - 1].size_bytes;
        const size_t right = results[boundary.index].size_bytes;

        std::vector<double> baseline_samples{};
        baseline_samples.push_back(results[boundary.index - 1].cycles_per_element);

        double previous = results[boundary.index - 1].cycles_per_element;
        for (size_t offset = 2; offset <= config_.refinement_samples && offset <= boundary.index; ++offset) {
            const double current = results[boundary.index - offset].cycles_per_element;
            if (std::abs(current - previous) / previous < config_.baseline_stability_threshold) {
                baseline_samples.push_back(current);
                previous = current;
            } else {
                break;
            }
        }

        const double baseline_latency =
            std::accumulate(baseline_samples.begin(), baseline_samples.end(), 0.0) / static_cast<double>(baseline_samples.size());

        double refinement_growth_factor = growth_factor_for(right);
        if (right >= kL1MaxSize && right < kL2MaxSize) {
            refinement_growth_factor *= config_.l2_refinement_growth_multiplier;
        }

        BoundaryAnalyzer analyzer{BoundaryAnalyzerConfig{refinement_growth_factor, 3}};
        return analyzer.refine_boundary(
            left, right, config_.precision, [this](size_t size) { return do_single_measurement(size).cycles_per_element; },
            baseline_latency);
    }

    void flush_cache_and_warmup(CacheProfilerList& list, size_t count) const {
        list.flush_from_cache();

        for (size_t iteration = 0; iteration < config_.warmup_iterations; ++iteration) {
            volatile CacheProfilerList::Element* element = list.first();
            for (size_t index = 0; index < count; ++index) {
                element = element->next;
            }
        }

        platform::arch::mfence();
        platform::arch::lfence();
    }

    static size_t level_index(CacheLevel level) noexcept { return static_cast<size_t>(level); }

    static const char* level_name(CacheLevel level) noexcept {
        switch (level) {
        case CacheLevel::l1d:
            return "L1d";
        case CacheLevel::l2:
            return "L2";
        case CacheLevel::l3:
            return "L3";
        }
        return "unknown";
    }

    double growth_factor_for(size_t size_bytes) const noexcept {
        if (size_bytes < kL1MaxSize) {
            return config_.l1_growth_factor;
        }
        if (size_bytes < kL2MaxSize) {
            return config_.l2_growth_factor;
        }
        return config_.l3_growth_factor;
    }
};

} // namespace silicon_probe::cache
