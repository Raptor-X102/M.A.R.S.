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
    
    data.l1d_size = CacheMeasurer::refine_boundary(results, detect_boundary(results));
    
    if (data.l1d_size) {
        LOG_INFO_STREAM << "L1d size detected: " << *data.l1d_size << " bytes";
    } else {
        LOG_WARNING("L1d size detection failed");
    }
}

void CacheMeasurer::measure_l2(CPUInfoData& data) {
    LOG_INFO("=== Measuring L2 ===");
    
    auto results = measure_range(
        config_.l1_max,
        config_.l2_max);
    
    data.l2_size = CacheMeasurer::refine_boundary(results, detect_boundary(results));
    
    if (data.l2_size) {
        LOG_INFO_STREAM << "L2 size detected: " << *data.l2_size << " bytes";
    } else {
        LOG_WARNING("L2 size detection failed");
    }
}

void CacheMeasurer::measure_l3(CPUInfoData& data) {
    LOG_INFO("=== Measuring L3 ===");
    
    auto results = measure_range(
        config_.l2_max,
        config_.l3_max);
    
    data.l3_size = CacheMeasurer::refine_boundary(results, detect_boundary(results));
    
    if (data.l3_size) {
        LOG_INFO_STREAM << "L3 size detected: " << *data.l3_size << " bytes";
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

CacheMeasurer::MeasurementResult CacheMeasurer::do_single_measurement(size_t size) {

    size_t count = size / cache_line_size_;
    CacheProfilerList list(cache_line_size_, count, config_.seed);
    volatile CacheProfilerList::Element* ptr = list.get_first();
    size_t max_iterations = config_.measure_iterations;
    if (size >= SIZE_WITH_DEF_ITER_SIZE) 
        max_iterations /= size / SIZE_WITH_DEF_ITER_SIZE;

    flush_cache_and_warmup(list, count);

    uint64_t start = arch::tick();
    for (size_t iterations = 0; iterations < config_.measure_iterations; ++iterations) {
        for (size_t elem_index = 0; elem_index < count; ++elem_index) {
            ptr = ptr->next;
        }
    }
    uint64_t end = arch::tick();
    
    double cpe = static_cast<double>(end - start) / count / config_.measure_iterations;
    
    LOG_DEBUG_STREAM << "Size=" << size << ", cycles/elem=" << cpe;
    return MeasurementResult(size, cpe);
}

std::vector<CacheMeasurer::MeasurementResult>::const_iterator 
CacheMeasurer::detect_boundary(const std::vector<MeasurementResult>& results) const {
    
    if (results.size() < 3) {
        return results.end();  
    }
    
    double base = results[0].cycles_per_element;
    
    for (auto it = results.begin(); it != results.end(); ++it) {
        double ratio = it->cycles_per_element / base;
        if (ratio > LATENCY_TRESHOLD) { 
            return it;
        }
    }
    
    LOG_WARNING_STREAM << "Measurements did not detect latency jump. Returning largest size";
    return std::prev(results.end());
}

std::optional<size_t> CacheMeasurer::refine_boundary(
    const std::vector<MeasurementResult>& results,
    std::vector<MeasurementResult>::const_iterator boundary_it) {
    
    if (boundary_it == results.begin() || boundary_it == results.end()) {
        return std::nullopt; 
    }
    
    size_t index = boundary_it - results.begin();
    return refine_boundary(results, index);
}

size_t CacheMeasurer::refine_boundary(
    const std::vector<MeasurementResult>& results,
    size_t boundary_index) {
    
    size_t left = results[boundary_index - 1].size_bytes;
    size_t right = results[boundary_index].size_bytes;
    
    // Фиксированный baseline - последний размер, который точно в кэше
    double baseline = results[boundary_index - 1].cycles_per_element;
    
    BoundaryAnalyzerConfig cfg;
    cfg.growth_factor = 1.5;      // допустим рост до 2х раз (L1: 5→10, L2: 30→60, L3: 100→200)
    cfg.test_samples = 3;
    
    BoundaryAnalyzer analyzer(cfg);
    
    auto measure = [this](size_t size) {
        return do_single_measurement(size).cycles_per_element;
    };
    
    return analyzer.refine_boundary(left, right, config_.precision, measure, baseline);
}
