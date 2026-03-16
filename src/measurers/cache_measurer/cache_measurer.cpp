#include "cache_measurer.hpp"
#include "cache_boundary.hpp"

CacheMeasurer::CacheMeasurer(const Config& config)
    : config_(config)
    , cache_line_size_(os::cache_line_size()) {
    
    LOG_INFO_STREAM << "[" << name() << "] Initialized with levels: "
                    << "L1=" << config_.levels.test(0)
                    << ", L2=" << config_.levels.test(1)
                    << ", L3=" << config_.levels.test(2);
}

void CacheMeasurer::measure(CPUInfoData& data) {
    LOG_INFO_STREAM << "[" << name() << "] Starting cache measurement";
    
    os::ScopedPriority priority_guard;
    os::ScopedFrequencyLock freq_guard;
    
    data.cache_line_size = cache_line_size_;

    if (config_.levels.test(0)) {
        measure_l1(data);
    }
    
    if (config_.levels.test(1)) {
        measure_l2(data);
    }
    
    if (config_.levels.test(2)) {
        measure_l3(data);
    }
    
    LOG_INFO_STREAM << "[" << name() << "] Cache measurement complete";
}

void CacheMeasurer::measure_l1(CPUInfoData& data) {
    LOG_INFO("=== Measuring L1d ===");
    
    auto results = measure_range(
        config_.cache_min_size * cache_line_size_,
        config_.l1_max);
    
    auto boundary = detect_boundary(results);
    
    if (boundary.index > 0 && boundary.index < results.size()) {
        data.l1d_size = refine_boundary(results, boundary);
        data.cache_line_size = cache_line_size_;
        LOG_INFO_STREAM << "L1d size detected: " << *data.l1d_size << " bytes";
    } else if (boundary.index == 0) {
        data.l1d_size = results[0].size_bytes;
        LOG_WARNING_STREAM << "L1d may be smaller than " << *data.l1d_size << " bytes";
    } else {
        LOG_WARNING("L1d size detection failed");
    }
}

void CacheMeasurer::measure_l2(CPUInfoData& data) {
    LOG_INFO("=== Measuring L2 ===");
    
    auto results = measure_range(
        config_.l1_max,
        config_.l2_max);
    
    auto boundary = detect_boundary(results);
    
    if (boundary.index > 0 && boundary.index < results.size()) {
        data.l2_size = refine_boundary(results, boundary);
        data.cache_line_size = cache_line_size_;
        LOG_INFO_STREAM << "L2 size detected: " << *data.l2_size << " bytes";
    } else if (boundary.index == 0) {
        data.l2_size = results[0].size_bytes;
        LOG_WARNING_STREAM << "L2 may be smaller than " << *data.l2_size << " bytes";
    } else {
        LOG_WARNING("L2 size detection failed");
    }
}

void CacheMeasurer::measure_l3(CPUInfoData& data) {
    LOG_INFO("=== Measuring L3 ===");
    
    auto results = measure_range(
        config_.l2_max,
        config_.l3_max);
    
    auto boundary = detect_boundary(results);
   
    if (boundary.index > 0 && boundary.index < results.size()) {
        data.l3_size = refine_boundary(results, boundary);
        data.cache_line_size = cache_line_size_;
        LOG_INFO_STREAM << "L3 size detected: " << *data.l3_size << " bytes";
    } else if (boundary.index == 0) {
        data.l3_size = results[0].size_bytes;
        LOG_WARNING_STREAM << "L3 may be smaller than " << *data.l3_size << " bytes";
    } else {
        LOG_WARNING("L3 size detection failed");
    }
}

void CacheMeasurer::flush_cache_and_warmup(CacheProfilerList& list, size_t count) {
    list.flush_from_cache();
    
    for (size_t i = 0; i < config_.warmup_iterations; ++i) {
        volatile CacheProfilerList::Element* ptr = list.get_first();
        for (size_t j = 0; j < count; ++j) {
            ptr = ptr->next;
        }
    }
    
    arch::mfence();
    arch::lfence();
}

std::vector<CacheMeasurer::MeasurementResult> CacheMeasurer::measure_range(
    size_t min_size, size_t max_size) {
    
    std::vector<MeasurementResult> results;
    
    results.reserve(static_cast<size_t>(std::log2(max_size / min_size)) + 1);
    
    for (size_t size = min_size; size <= max_size; size *= 2) {
        results.emplace_back(size, do_single_measurement(size).cycles_per_element);
       
        LOG_INFO_STREAM << "Size=" << results.back().size_bytes 
                        << ", cycles/elem=" << results.back().cycles_per_element;
    }
    
    return results;
}

BoundaryResult CacheMeasurer::detect_boundary(const std::vector<MeasurementResult>& results) const {
    BoundaryResult res;
    res.index = results.size();
    res.baseline_latency = 0.0;
    
    if (results.size() < 3) return res;
    
    // First 3 points are definitely in L1
    double sum = 0.0;
    for (size_t i = 0; i < 3; ++i) {
        sum += results[i].cycles_per_element;
    }
    res.baseline_latency = sum / 3.0;
    
    // Dynamic threshold based on current size
    for (size_t i = 1; i < results.size(); ++i) {
        double ratio = results[i].cycles_per_element / res.baseline_latency;
        
        // Determine threshold based on approximate cache level
        double threshold;
        if (results[i].size_bytes < L1_MAX_SIZE) {
            threshold = L1_GROWTH_FACTOR;  // 2.0
        } else if (results[i].size_bytes < L2_MAX_SIZE) {
            threshold = L2_GROWTH_FACTOR;  // 2.2
        } else {
            threshold = L3_GROWTH_FACTOR;  // 2.5
        }
        
        if (ratio >= threshold) {
            res.index = i;
            break;
        }
    }
    
    return res;
}

CacheMeasurer::MeasurementResult CacheMeasurer::do_single_measurement(size_t size) {
    size_t count = size / cache_line_size_;
    
    // Calculate iterations to achieve target total accesses
    size_t iterations = TARGET_ACCESSES / count;
    if (iterations < MIN_ITERATIONS) iterations = MIN_ITERATIONS;
    if (iterations > MAX_ITERATIONS) iterations = MAX_ITERATIONS;
    
    CacheProfilerList list(cache_line_size_, count, config_.seed);
    flush_cache_and_warmup(list, count);
    
    volatile CacheProfilerList::Element* ptr = list.get_first();
    uint64_t start = arch::tick();
    for (size_t iter = 0; iter < iterations; ++iter) {
        for (size_t i = 0; i < count; ++i) {
            ptr = ptr->next;
        }
    }
    uint64_t end = arch::tick();
    
    double cpe = static_cast<double>(end - start) / (count * iterations);
    LOG_DEBUG_STREAM << "Size=" << size << ", cycles/elem=" << cpe;
    return MeasurementResult(size, cpe);
}

size_t CacheMeasurer::refine_boundary(const std::vector<MeasurementResult>& results,
                                      BoundaryResult& result) {
    
    size_t left = results[result.index - 1].size_bytes;
    size_t right = results[result.index].size_bytes;
    
    // Collect stable baseline samples from points before boundary
    std::vector<double> baseline_samples;
    double prev = results[result.index - 1].cycles_per_element;
    baseline_samples.push_back(prev);
    
    for (size_t i = 2; i <= BASELINE_SAMPLES && i <= result.index; ++i) {
        double cur = results[result.index - i].cycles_per_element;
        if (std::abs(cur - prev) / prev < STABILITY_THRESHOLD) {
            baseline_samples.push_back(cur);
            prev = cur;
        } else {
            break;
        }
    }
    
    double baseline_latency = std::accumulate(baseline_samples.begin(), baseline_samples.end(), 0.0)
                              / baseline_samples.size();
    
    // Growth factor for refinement (more aggressive than detection)
    double growth_factor;
    if (right < L1_MAX_SIZE) {
        growth_factor = L1_GROWTH_FACTOR;           // 2.0
    } else if (right < L2_MAX_SIZE) {
        growth_factor = L2_GROWTH_FACTOR * 1.5;    // 2.2 * 1.15 = 2.53
    } else {
        growth_factor = L3_GROWTH_FACTOR;           // 2.5
    }
    
    BoundaryAnalyzerConfig cfg;
    cfg.growth_factor = growth_factor;
    cfg.test_samples = 3;
    
    BoundaryAnalyzer analyzer(cfg);
    
    auto measure = [this](size_t size) {
        return do_single_measurement(size).cycles_per_element;
    };
    
    return analyzer.refine_boundary(left, right, config_.precision, measure, baseline_latency);
}
