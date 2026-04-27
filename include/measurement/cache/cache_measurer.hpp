#pragma once

#include "measurement/cache/boundary_analyzer.hpp"
#include "measurement/cache/cache_profiler_list.hpp"
#include "infra/logging.hpp"
#include "measurement/core/measurer.hpp"
#include "platform/arch.hpp"
#include "platform/os.hpp"
#include "platform/pmc.hpp"
#include "platform/events_discovery.hpp"
#include "shared_types/cache_types.hpp"

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
#include <memory>

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

public:
    CacheMeasurer() : CacheMeasurer(Config{}) {}

    explicit CacheMeasurer(Config config) : config_(std::move(config)), cache_line_size_(platform::cache_line_size()) {
        SPDLOG_INFO("[{}] configured with levels: L1={}, L2={}, L3={}", name(),
                    config_.levels.test(level_index(CacheLevel::l1d)),
                    config_.levels.test(level_index(CacheLevel::l2)),
                    config_.levels.test(level_index(CacheLevel::l3)));
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

        auto pmc = open_pmc_for_level(level);
        bool use_misses = (pmc != nullptr);

        auto results = measure_range(min_size, max_size, pmc);
        if (results.empty()) {
            SPDLOG_WARN("No measurements collected for {}", level_name(level));
            return;
        }

        BoundaryResult boundary_latency = detect_latency_boundary(results);
        size_t size_latency = 0;
        if (boundary_latency.index > 0 && boundary_latency.index < results.size()) {
            size_latency = refine_boundary_latency(results, boundary_latency);
        }

        size_t size_misses = 0;
        if (use_misses) {
            size_t miss_index = detect_miss_rate_boundary(results, level);
            if (miss_index > 0 && miss_index < results.size()) {
                size_misses = refine_boundary_misses(results, miss_index, level, pmc);
            } else {
                SPDLOG_WARN("Miss-rate-based boundary detection failed for {}", level_name(level));
                use_misses = false;
            }
        }

        size_t final_size = 0;
        if (use_misses && size_misses > 0) {
            if (size_latency > 0) {
                double diff = std::abs(static_cast<double>(size_latency) - static_cast<double>(size_misses));
                double avg = (size_latency + size_misses) / 2.0;
                if (diff / avg <= config_.decision_tolerance) {
                    final_size = static_cast<size_t>(std::round(avg));
                    SPDLOG_INFO("Both methods agree within {}%: latency={}, misses={}, final={}",
                                config_.decision_tolerance * 100, size_latency, size_misses, final_size);
                } else {
                    final_size = size_misses;
                    SPDLOG_INFO("Misses method used (more accurate), latency gave {} but misses gave {}",
                                size_latency, size_misses);
                }
            } else {
                final_size = size_misses;
                SPDLOG_INFO("Only misses method available, result={}", final_size);
            }
        } else if (size_latency > 0) {
            final_size = size_latency;
            SPDLOG_INFO("Only latency method available, result={}", final_size);
        } else {
            final_size = results.front().size_bytes;
            SPDLOG_WARN("No reliable boundary, using smallest size={}", final_size);
        }

        auto& target = data.*target_field;
        if (final_size > 0) {
            target = final_size;
            SPDLOG_INFO("{} size detected: {} bytes", level_name(level), *target);
        } else {
            SPDLOG_WARN("{} size detection failed", level_name(level));
        }
    }

    std::unique_ptr<platform::pmc::PmcGroup> open_pmc_for_level(CacheLevel level) const {
        auto events = platform::discover_cache_miss_events(level);
        if (events.empty()) {
            SPDLOG_DEBUG("No miss events found for {}, using latency-only", level_name(level));
            return nullptr;
        }
        auto pmc = platform::pmc::PmcGroup::create_raw(events);
        if (!pmc) {
            SPDLOG_WARN("Failed to open PMC for {} miss events, fallback to latency", level_name(level));
            return nullptr;
        }

        std::string events_str;
        for (size_t i = 0; i < events.size(); ++i) {
            if (i > 0) events_str += ", ";
            events_str += events[i];
        }
        SPDLOG_INFO("Opened PMC for {} with events: {}", level_name(level), events_str);
        return pmc;
    }

    std::vector<MeasurementResult> measure_range(size_t min_size, size_t max_size,
                                                  std::unique_ptr<platform::pmc::PmcGroup>& pmc) {
        std::vector<MeasurementResult> results;
        for (size_t size = min_size; size <= max_size; size *= 2) {
            if (pmc) {
                results.push_back(do_single_measurement_with_pmc(size, *pmc));
                SPDLOG_INFO("Size={}, cycles/elem={}, miss_rate={:.6f}", results.back().size_bytes,
                            results.back().cycles_per_element, results.back().miss_rate);
            } else {
                results.push_back(do_single_measurement_without_pmc(size));
                SPDLOG_INFO("Size={}, cycles/elem={}", results.back().size_bytes, results.back().cycles_per_element);
            }
            if (size > max_size / 2) {
                break;
            }
        }
        return results;
    }

    MeasurementResult do_single_measurement_without_pmc(size_t size) {
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

        MeasurementResult result;
        result.size_bytes = size;
        result.cycles_per_element = static_cast<double>(end - start) / static_cast<double>(count * iterations);
        result.has_pmc = false;
        result.miss_rate = 0.0;
        return result;
    }

    MeasurementResult do_single_measurement_with_pmc(size_t size, platform::pmc::PmcGroup& pmc) {
        const size_t count = size / cache_line_size_;
        if (count == 0) {
            throw std::invalid_argument("Measurement size must be at least one cache line");
        }

        size_t iterations = config_.target_accesses / count;
        iterations = std::max(iterations, config_.min_iterations);
        iterations = std::min(iterations, config_.max_iterations);
        uint64_t total_loads = count * iterations;

        CacheProfilerList list{cache_line_size_, count, config_.seed};
        flush_cache_and_warmup(list, count);

        volatile CacheProfilerList::Element* element = list.first();

        pmc.reset();
        pmc.enable();

        const uint64_t start = platform::arch::tick();
        for (size_t iteration = 0; iteration < iterations; ++iteration) {
            for (size_t index = 0; index < count; ++index) {
                element = element->next;
            }
        }
        const uint64_t end = platform::arch::tick();

        pmc.disable();
        auto values = pmc.read();

        MeasurementResult result;
        result.size_bytes = size;
        result.cycles_per_element = static_cast<double>(end - start) / static_cast<double>(total_loads);
        result.has_pmc = true;
        if (values.valid && !values.values.empty()) {
            result.miss_rate = static_cast<double>(values.values[0]) / static_cast<double>(total_loads);
        } else {
            result.miss_rate = 0.0;
            SPDLOG_WARN("Failed to read PMC values for size {}", size);
        }
        return result;
    }

    BoundaryResult detect_latency_boundary(const std::vector<MeasurementResult>& results) const {
        BoundaryResult boundary;
        boundary.index = results.size();

        if (results.size() < 3) {
            return boundary;
        }

        double baseline_sum = 0.0;
        for (size_t i = 0; i < 3; ++i) {
            baseline_sum += results[i].cycles_per_element;
        }
        double baseline = baseline_sum / 3.0;
        boundary.baseline_value = baseline;

        for (size_t i = 1; i < results.size(); ++i) {
            double ratio = results[i].cycles_per_element / baseline;
            if (ratio >= growth_factor_for(results[i].size_bytes)) {
                boundary.index = i;
                break;
            }
        }
        return boundary;
    }

    size_t detect_miss_rate_boundary(const std::vector<MeasurementResult>& results, CacheLevel level) const {
        if (results.size() < 3) return results.size();

        double threshold = 0.0;
        double growth = 0.0;
        switch (level) {
            case CacheLevel::l1d:
                threshold = config_.l1_miss_rate_threshold;
                growth = config_.l1_miss_growth_factor;
                break;
            case CacheLevel::l2:
                threshold = config_.l2_miss_rate_threshold;
                growth = config_.l2_miss_growth_factor;
                break;
            case CacheLevel::l3:
                threshold = config_.l3_miss_rate_threshold;
                growth = config_.l3_miss_growth_factor;
                break;
        }

        double baseline = (results[0].miss_rate + results[1].miss_rate + results[2].miss_rate) / 3.0;
        if (baseline < 1e-12) baseline = 1e-12;

        for (size_t i = 1; i < results.size(); ++i) {
            double miss_rate = results[i].miss_rate;
            if (miss_rate >= threshold && miss_rate >= baseline * growth) {
                return i;
            }
        }
        return results.size();
    }

    size_t refine_boundary_latency(const std::vector<MeasurementResult>& results, const BoundaryResult& boundary) {
        const size_t left = results[boundary.index - 1].size_bytes;
        const size_t right = results[boundary.index].size_bytes;

        std::vector<double> baseline_samples;
        baseline_samples.push_back(results[boundary.index - 1].cycles_per_element);

        double previous = baseline_samples.back();
        for (size_t offset = 2; offset <= config_.refinement_samples && offset <= boundary.index; ++offset) {
            double current = results[boundary.index - offset].cycles_per_element;
            if (std::abs(current - previous) / previous < config_.baseline_stability_threshold) {
                baseline_samples.push_back(current);
                previous = current;
            } else {
                break;
            }
        }

        double baseline = std::accumulate(baseline_samples.begin(), baseline_samples.end(), 0.0)
                          / static_cast<double>(baseline_samples.size());

        double refinement_growth_factor = growth_factor_for(right);
        if (right >= config_.l1_max && right < config_.l2_max) {
            refinement_growth_factor *= config_.l2_refinement_growth_multiplier;
        }

        BoundaryAnalyzerConfig analyzer_config;
        analyzer_config.growth_factor = refinement_growth_factor;
        analyzer_config.test_samples = 3;
        BoundaryAnalyzer analyzer(analyzer_config);

        return analyzer.refine_boundary(
            left, right, config_.precision,
            [this](size_t size) -> double {
                return do_single_measurement_without_pmc(size).cycles_per_element;
            },
            baseline);
    }

    size_t refine_boundary_misses(const std::vector<MeasurementResult>& results, size_t miss_index,
                                  CacheLevel level, std::unique_ptr<platform::pmc::PmcGroup>& pmc) {
        const size_t left = results[miss_index - 1].size_bytes;
        const size_t right = results[miss_index].size_bytes;

        double baseline = results[miss_index - 1].miss_rate;
        if (baseline < 1e-12) baseline = 1e-12;

        double refinement_growth_factor = 0.0;
        switch (level) {
            case CacheLevel::l1d:
                refinement_growth_factor = config_.l1_miss_growth_factor;
                break;
            case CacheLevel::l2:
                refinement_growth_factor = config_.l2_miss_growth_factor;
                break;
            case CacheLevel::l3:
                refinement_growth_factor = config_.l3_miss_growth_factor;
                break;
        }

        BoundaryAnalyzerConfig analyzer_config;
        analyzer_config.growth_factor = refinement_growth_factor;
        analyzer_config.test_samples = 3;
        BoundaryAnalyzer analyzer(analyzer_config);

        return analyzer.refine_boundary(
            left, right, config_.precision,
            [this, &pmc](size_t size) -> double {
                return do_single_measurement_with_pmc(size, *pmc).miss_rate;
            },
            baseline);
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

    double growth_factor_for(size_t size_bytes) const noexcept {
        if (size_bytes < config_.l1_max) return config_.l1_growth_factor;
        if (size_bytes < config_.l2_max) return config_.l2_growth_factor;
        return config_.l3_growth_factor;
    }

    static size_t level_index(CacheLevel level) noexcept { return static_cast<size_t>(level); }

    static const char* level_name(CacheLevel level) noexcept {
        switch (level) {
        case CacheLevel::l1d: return "L1d";
        case CacheLevel::l2:  return "L2";
        case CacheLevel::l3:  return "L3";
        }
        return "unknown";
    }
};

} // namespace silicon_probe::cache
