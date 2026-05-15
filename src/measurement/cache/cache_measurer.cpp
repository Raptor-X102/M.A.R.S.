// measurement/cache/cache_measurer.cpp
#include "measurement/cache/cache_measurer.hpp"

#include <numeric>
#include <iostream>

namespace silicon_probe::cache {

CacheMeasurer::CacheMeasurer() : CacheMeasurer(Config{}) {}

CacheMeasurer::CacheMeasurer(Config config)
    : config_(std::move(config)), cache_line_size_(platform::cache_line_size()) {
    validateConfig();
    SPDLOG_DEBUG(
        "[{}] configured with levels: L1={}, L2={}, L3={}, huge_pages={}",
        name(),
        config_.levels.test(level_index(CacheLevel::l1d)),
        config_.levels.test(level_index(CacheLevel::l2)),
        config_.levels.test(level_index(CacheLevel::l3)),
        config_.use_huge_pages
    );
}

std::string_view CacheMeasurer::name() const noexcept { return "cache"; }

void CacheMeasurer::validateConfig() {
    if (config_.l1_max == 0) config_.l1_max = kL1MaxSize;
    if (config_.l2_max == 0) config_.l2_max = kL2MaxSize;
    if (config_.l3_max == 0) config_.l3_max = kL3MaxSize;
    
    if (config_.l1_max >= config_.l2_max) {
        SPDLOG_WARN("l1_max ({}) >= l2_max ({}), adjusting", config_.l1_max, config_.l2_max);
        config_.l2_max = config_.l1_max * 2;
    }
    if (config_.l2_max >= config_.l3_max) {
        SPDLOG_WARN("l2_max ({}) >= l3_max ({}), adjusting", config_.l2_max, config_.l3_max);
        config_.l3_max = config_.l2_max * 2;
    }
    
    size_t min_valid_size = config_.cache_min_lines * cache_line_size_;
    if (min_valid_size == 0) {
        config_.cache_min_lines = kDefaultCacheMinLines;
        min_valid_size = config_.cache_min_lines * cache_line_size_;
    }
    if (config_.l1_max < min_valid_size) {
        config_.l1_max = min_valid_size;
    }
    
    config_.warmup_iterations = std::max<size_t>(1, config_.warmup_iterations);
    config_.precision = std::max<size_t>(1, config_.precision);
    config_.target_accesses = std::max<size_t>(1, config_.target_accesses);
    config_.min_iterations = std::max<size_t>(1, config_.min_iterations);
    config_.max_iterations = std::max(config_.max_iterations, config_.min_iterations);
    config_.refinement_samples = std::max<size_t>(1, config_.refinement_samples);
    
    config_.decision_tolerance = std::clamp(config_.decision_tolerance, 0.0, 1.0);
    config_.baseline_stability_threshold = std::clamp(config_.baseline_stability_threshold, 0.0, 1.0);
    
    config_.l1_growth_factor = std::max(1.01, config_.l1_growth_factor);
    config_.l2_growth_factor = std::max(1.01, config_.l2_growth_factor);
    config_.l3_growth_factor = std::max(1.01, config_.l3_growth_factor);
    
    config_.l1_miss_rate_threshold = std::clamp(config_.l1_miss_rate_threshold, 0.0, 1.0);
    config_.l2_miss_rate_threshold = std::clamp(config_.l2_miss_rate_threshold, 0.0, 1.0);
    config_.l3_miss_rate_threshold = std::clamp(config_.l3_miss_rate_threshold, 0.0, 1.0);
    config_.l1_miss_growth_factor = std::max(1.01, config_.l1_miss_growth_factor);
    config_.l2_miss_growth_factor = std::max(1.01, config_.l2_miss_growth_factor);
    config_.l3_miss_growth_factor = std::max(1.01, config_.l3_miss_growth_factor);
    
    if (config_.levels.none()) {
        config_.levels.set(level_index(CacheLevel::l1d));
        config_.levels.set(level_index(CacheLevel::l2));
        config_.levels.set(level_index(CacheLevel::l3));
    }
}

void CacheMeasurer::measure(shared_types::CpuInfoData& data) {
    SPDLOG_INFO("[{}] starting cache measurement", name());
    platform::ScopedMeasurementEnvironment environment{config_.environment};

    data.cache_line_size = cache_line_size_;

    if (config_.levels.test(level_index(CacheLevel::l3)))
        reusable_max_size_ = config_.l3_max / cache_line_size_;
    else if (config_.levels.test(level_index(CacheLevel::l2)))
        reusable_max_size_ = config_.l2_max / cache_line_size_;
    else if (config_.levels.test(level_index(CacheLevel::l1d)))
        reusable_max_size_ = config_.l1_max / cache_line_size_;

    const size_t l1_min = config_.cache_min_lines * cache_line_size_;
    if (config_.levels.test(level_index(CacheLevel::l1d))) {
        measure_level(data, CacheLevel::l1d, l1_min, config_.l1_max, &shared_types::CpuInfoData::l1d_size);
    }
    if (config_.levels.test(level_index(CacheLevel::l2))) {
        measure_level(data, CacheLevel::l2, config_.l1_max, config_.l2_max, &shared_types::CpuInfoData::l2_size);
    }
    if (config_.levels.test(level_index(CacheLevel::l3))) {
        measure_level(data, CacheLevel::l3, config_.l2_max, config_.l3_max, &shared_types::CpuInfoData::l3_size);
    }

    SPDLOG_INFO("[{}] cache measurement complete", name());
}

void CacheMeasurer::measure_level(
    shared_types::CpuInfoData& data,
    CacheLevel level,
    size_t min_size,
    size_t max_size,
    std::optional<size_t> shared_types::CpuInfoData::* target_field
) {
    const char* levelname = level_name(level);
    SPDLOG_INFO("[{}] measuring {}", name(), levelname);

    if (min_size == 0 || min_size > max_size) {
        SPDLOG_WARN("Skipping invalid range for {}: min={}, max={}", levelname, min_size, max_size);
        return;
    }

    auto pmc        = open_pmc_for_level(level, data);
    bool use_misses = (pmc != nullptr);

    auto results = measure_range(min_size, max_size, pmc);
    if (results.empty()) {
        SPDLOG_WARN("No measurements collected for {}", levelname);
        return;
    }

    BoundaryResult boundary_latency = detect_latency_boundary(results);
    size_t size_latency             = 0;
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
            double avg  = (size_latency + size_misses) / 2.0;
            if (diff / avg <= config_.decision_tolerance) {
                final_size = static_cast<size_t>(std::round(avg));
                SPDLOG_DEBUG(
                    "Both methods agree within {}%: latency={}, misses={}, final={}",
                    config_.decision_tolerance * 100,
                    size_latency,
                    size_misses,
                    final_size
                );
            } else {
                final_size = size_misses;
                SPDLOG_DEBUG(
                    "Misses method used (more accurate), latency gave {} but misses gave {}",
                    size_latency,
                    size_misses
                );
            }
        } else {
            final_size = size_misses;
            SPDLOG_DEBUG("Only misses method available, result={}", final_size);
        }
    } else if (size_latency > 0) {
        final_size = size_latency;
        SPDLOG_DEBUG("Only latency method available, result={}", final_size);
    } else {
        final_size = results.front().size_bytes;
        SPDLOG_WARN("No reliable boundary, using smallest size={}", final_size);
    }

    auto& target = data.*target_field;
    if (final_size > 0) {
        target = final_size;
        SPDLOG_INFO("{} size detected: {} bytes", levelname, *target);
    } else {
        SPDLOG_WARN("{} size detection failed", levelname);
    }
}

std::unique_ptr<platform::pmc::PmcGroup>
CacheMeasurer::open_pmc_for_level(CacheLevel level, shared_types::CpuInfoData& data) const {
    auto events = platform::discover_cache_miss_events(level, data);
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
        if (i > 0)
            events_str += ", ";
        events_str += events[i];
    }
    SPDLOG_DEBUG("Opened PMC for {} with events: {}", level_name(level), events_str);
    return pmc;
}

std::vector<CacheMeasurer::MeasurementResult>
CacheMeasurer::measure_range(size_t min_size, size_t max_size, std::unique_ptr<platform::pmc::PmcGroup>& pmc) {
    if (!reusable_list_) {
        SPDLOG_DEBUG("reusable_list_ = {}", reusable_max_size_);
        if (config_.use_huge_pages) {
            reusable_list_ = std::make_unique<CacheProfilerList>(
                cache_line_size_, reusable_max_size_,
                CacheProfilerList::MemoryType::huge_page
            );
        } else {
            reusable_list_ = std::make_unique<CacheProfilerList>(
                cache_line_size_, reusable_max_size_,
                CacheProfilerList::MemoryType::aligned 
            );
        }
    }

    std::vector<MeasurementResult> results;
    for (size_t size = min_size; size <= max_size; size *= 2) {
        const size_t count = std::max<size_t>(1, size / cache_line_size_);

        // Prepare the list for exactly 'count' elements with current seed
        // Use a deterministic seed derived from config_.seed and maybe size
        unsigned int seed = config_.seed ^ static_cast<unsigned int>(count);
        reusable_list_->prepare(count, seed);

        MeasurementResult result;
        if (pmc) {
            result = do_single_measurement_with_pmc(reusable_list_.get(), count, *pmc);
        } else {
            result = do_single_measurement_without_pmc(reusable_list_.get(), count);
        }
        result.size_bytes = size;

        SPDLOG_INFO("Size={}, cycles/elem={}, miss_rate={:.6f}",
                     result.size_bytes, result.cycles_per_element, result.miss_rate);

        results.push_back(result);
    }
    return results;
}

CacheMeasurer::MeasurementResult
CacheMeasurer::do_single_measurement_without_pmc(CacheProfilerList* list, size_t count) {
    return measure_impl(list, count,
        []() noexcept {},
        [](uint64_t /*total_loads*/) noexcept -> std::optional<double> {
            return std::nullopt;
        }
    );
}

CacheMeasurer::MeasurementResult
CacheMeasurer::do_single_measurement_with_pmc(CacheProfilerList* list, size_t count, platform::pmc::PmcGroup& pmc) {
    return measure_impl(list, count,
        [&pmc]() {
            pmc.reset();
            pmc.enable();
        },
        [&pmc](uint64_t total_loads) -> std::optional<double> {
            pmc.disable();
            auto values = pmc.read();
            if (values.valid && !values.values.empty()) {
                return static_cast<double>(values.values[0]) / static_cast<double>(total_loads);
            }
            return std::nullopt;
        }
    );
}

CacheMeasurer::BoundaryResult CacheMeasurer::detect_latency_boundary(
    const std::vector<MeasurementResult>& results
) const {
    BoundaryResult boundary;
    boundary.index = results.size();

    if (results.size() < config_.refinement_samples) {
        return boundary;
    }

    // Use median of first three points instead of mean
    std::vector<double> baseline_samples;
    baseline_samples.reserve(config_.refinement_samples);
    for (size_t i = 0; i < config_.refinement_samples; ++i) {
        baseline_samples.push_back(results[i].cycles_per_element);
    }
    double baseline = statistics::compute_median(std::move(baseline_samples));
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

size_t CacheMeasurer::detect_miss_rate_boundary(const std::vector<MeasurementResult>& results, CacheLevel level) const {
    if (results.size() < config_.refinement_samples)
        return results.size();

    double threshold = 0.0;
    double growth    = 0.0;
    switch (level) {
        case CacheLevel::l1d:
            threshold = config_.l1_miss_rate_threshold;
            growth    = config_.l1_miss_growth_factor;
            break;
        case CacheLevel::l2:
            threshold = config_.l2_miss_rate_threshold;
            growth    = config_.l2_miss_growth_factor;
            break;
        case CacheLevel::l3:
            threshold = config_.l3_miss_rate_threshold;
            growth    = config_.l3_miss_growth_factor;
            break;
    }

    // Median of first three miss rates
    std::vector<double> baseline_samples;
    baseline_samples.reserve(config_.refinement_samples);
    for (size_t i = 0; i < config_.refinement_samples; ++i) {
        baseline_samples.push_back(results[i].miss_rate);
    }
    double baseline = statistics::compute_median(std::move(baseline_samples));
    if (baseline < 1e-12)
        baseline = 1e-12;

    for (size_t i = 1; i < results.size(); ++i) {
        double miss_rate = results[i].miss_rate;
        if (miss_rate >= threshold && miss_rate >= baseline * growth) {
            return i;
        }
    }
    return results.size();
}

size_t
CacheMeasurer::refine_boundary_latency(const std::vector<MeasurementResult>& results, const BoundaryResult& boundary) {
    const size_t left  = results[boundary.index - 1].size_bytes;
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

    double baseline = statistics::compute_median(std::move(baseline_samples));

    double refinement_growth_factor = growth_factor_for(right);
    if (right >= config_.l1_max && right < config_.l2_max) {
        refinement_growth_factor *= config_.l2_refinement_growth_multiplier;
    }

    return refine_boundary(
        left,
        right,
        config_.precision,
        refinement_growth_factor,
        [this](size_t size) -> double {
            size_t count = size / cache_line_size_;
            if (count == 0) count = 1;
            unsigned int seed = config_.seed ^ static_cast<unsigned int>(count);
            reusable_list_->prepare(count, seed);
            auto result = measure_impl(reusable_list_.get(), count,
                []() noexcept {},
                [](uint64_t) noexcept -> std::optional<double> { return std::nullopt; }
            );
            return result.cycles_per_element;
        },
        baseline
    );
}

size_t CacheMeasurer::refine_boundary_misses(
    const std::vector<MeasurementResult>& results,
    size_t miss_index,
    CacheLevel level,
    std::unique_ptr<platform::pmc::PmcGroup>& pmc
) {
    const size_t left  = results[miss_index - 1].size_bytes;
    const size_t right = results[miss_index].size_bytes;

    std::vector<double> baseline_samples;
    baseline_samples.push_back(results[miss_index - 1].miss_rate);
    double previous = baseline_samples.back();

    for (size_t offset = 2; offset <= config_.refinement_samples && offset <= miss_index; ++offset) {
        double current = results[miss_index - offset].miss_rate;
        if (std::abs(current - previous) / previous < config_.baseline_stability_threshold) {
            baseline_samples.push_back(current);
            previous = current;
        } else {
            break;
        }
    }

    double baseline = statistics::compute_median(std::move(baseline_samples));
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

    return refine_boundary(
        left,
        right,
        config_.precision,
        refinement_growth_factor,
        [this, &pmc](size_t size) -> double {
            size_t count = size / cache_line_size_;
            if (count == 0) count = 1;
            unsigned int seed = config_.seed ^ static_cast<unsigned int>(count);
            reusable_list_->prepare(count, seed);
            auto result = measure_impl(reusable_list_.get(), count,
                [&pmc]() {
                    pmc->reset();
                    pmc->enable();
                },
                [&pmc](uint64_t total_loads) -> std::optional<double> {
                    pmc->disable();
                    auto values = pmc->read();
                    if (values.valid && !values.values.empty()) {
                        return static_cast<double>(values.values[0]) / static_cast<double>(total_loads);
                    }
                    return std::nullopt;
                }
            );
            return result.miss_rate;
        },
        baseline
    );
}

void CacheMeasurer::flush_cache_and_warmup(CacheProfilerList& list, size_t count) const {
    list.flush_from_cache();

    for (size_t iteration = 0; iteration < config_.warmup_iterations; ++iteration) {
        volatile CacheProfilerList::Element* element = list.first();
        for (size_t index = 0; index < count; ++index) {
            element = element->next;
        }
    }

    platform::arch::mfence();
}

double CacheMeasurer::growth_factor_for(size_t size_bytes) const noexcept {
    if (size_bytes <= config_.l1_max)
        return config_.l1_growth_factor;
    if (size_bytes < config_.l2_max)
        return config_.l2_growth_factor;
    return config_.l3_growth_factor;
}

size_t CacheMeasurer::level_index(CacheLevel level) noexcept { return static_cast<size_t>(level); }

const char* CacheMeasurer::level_name(CacheLevel level) noexcept {
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

}  // namespace silicon_probe::cache
