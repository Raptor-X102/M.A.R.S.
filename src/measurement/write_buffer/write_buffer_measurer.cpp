// measurement/write_buffer/write_buffer_measurer.cpp
#include "measurement/write_buffer/write_buffer_measurer.hpp"

#include <numeric>
#include <algorithm>
#include <cmath>

namespace silicon_probe::write_buffer {

WriteBufferMeasurer::WriteBufferMeasurer() : WriteBufferMeasurer(Config{}) {}

WriteBufferMeasurer::WriteBufferMeasurer(Config config) : config_(std::move(config)) {
    validateConfig();
    SPDLOG_DEBUG(
        "[{}] cfg: min_writes={} max_writes={} step={} samples_per_repeat={} repeats={}",
        name(),
        config_.min_writes,
        config_.max_writes,
        config_.writes_step,
        config_.iterations,
        config_.repeats
    );
}

std::string_view WriteBufferMeasurer::name() const noexcept { return "write_buffer"; }

void WriteBufferMeasurer::validateConfig() {
    if (config_.min_writes == 0) config_.min_writes = kDefaultMinWrites;
    if (config_.max_writes == 0) config_.max_writes = kDefaultMaxWrites;
    if (config_.min_writes > config_.max_writes) {
        std::swap(config_.min_writes, config_.max_writes);
    }
    if (config_.writes_step == 0) config_.writes_step = kDefaultWritesStep;
    if (config_.iterations == 0) config_.iterations = kDefaultIterations;
    if (config_.repeats == 0) config_.repeats = kDefaultRepeats;
    if (config_.warmup_iterations == 0) config_.warmup_iterations = kDefaultWarmupIterations;

    if (config_.latency_spike_ratio < 1.1) config_.latency_spike_ratio = 2.0;
    if (config_.latency_hold_ratio < 1.0) config_.latency_hold_ratio = 1.5;
    if (config_.stall_fallback_ratio < 0.0 || config_.stall_fallback_ratio > 1.0)
        config_.stall_fallback_ratio = 0.9;
    if (config_.baseline_window < 1) config_.baseline_window = 3;
    if (config_.stall_baseline_ratio < 1.0) config_.stall_baseline_ratio = 10.0;
    if (config_.stall_absolute_min < 0.0) config_.stall_absolute_min = 100.0;
    if (config_.stall_gradient_ratio < 1.0) config_.stall_gradient_ratio = 2.0;
    if (config_.stall_median_window < 1) config_.stall_median_window = 3;

    const size_t buffer_bytes = kBufferSizeMB * 1024 * 1024;
    const size_t num_elements = buffer_bytes / kBytesPerEntry;
    const size_t region_size = ((config_.max_writes * kCacheLineSize) + kCacheLineSize - 1) & ~(kCacheLineSize - 1);
    if ((config_.max_writes + 1) * region_size > num_elements * kBytesPerEntry) {
        SPDLOG_WARN("[{}] Buffer too small for max_writes={}, reducing", name(), config_.max_writes);
        config_.max_writes = (num_elements * kBytesPerEntry) / region_size - 1;
        if (config_.max_writes < config_.min_writes) {
            config_.min_writes = config_.max_writes;
        }
    }
}

void WriteBufferMeasurer::measure(shared_types::CpuInfoData& data) {
    SPDLOG_INFO("[{}] starting write buffer measurement", name());
    platform::ScopedMeasurementEnvironment env{config_.environment};

    auto events  = platform::discover_write_buffer_events(data);
    bool has_pmc = !events.empty();
    std::unique_ptr<platform::pmc::PmcGroup> pmc;
    size_t sb_idx = std::string::npos, bound_idx = std::string::npos;
    if (has_pmc) {
        pmc = platform::pmc::PmcGroup::create_raw(events);
        if (!pmc) {
            SPDLOG_WARN("[{}] PMC open failed, fallback to time-only", name());
            has_pmc = false;
        } else {
            for (size_t i = 0; i < events.size(); ++i) {
                if (events[i].find("resource_stalls.sb") != std::string::npos)
                    sb_idx = i;
                if (events[i].find("exe_activity.bound_on_stores") != std::string::npos)
                    bound_idx = i;
            }
        }
    }

    if (!has_pmc) {
        SPDLOG_WARN("[{}] No usable PMC events. Write buffer measurement requires PMC. Skipping.", name());
        return;
    }

    const size_t buffer_bytes = kBufferSizeMB * 1024 * 1024;
    const size_t num_elements = buffer_bytes / kBytesPerEntry;
    std::unique_ptr<int[]> fill_area(new int[num_elements]);
    std::unique_ptr<int[]> extra_area(new int[num_elements]);
    std::memset(fill_area.get(), 0, buffer_bytes);
    std::memset(extra_area.get(), 0, buffer_bytes);

    const size_t region_size = ((config_.max_writes * kCacheLineSize) + kCacheLineSize - 1) & ~(kCacheLineSize - 1);
    if ((config_.max_writes + 1) * region_size > num_elements * kBytesPerEntry) {
        SPDLOG_ERROR("[{}] Buffer too small for requested max_writes", name());
        return;
    }

    volatile int dummy = 0;
    SPDLOG_DEBUG("| writes | latency(ticks) | stddev |");
    for (const auto& ev : events)
        SPDLOG_DEBUG("|        | {} (per sample) |", ev);

    std::vector<size_t> writes_list;
    std::vector<WriteBufferResult> results;

    auto* fill_base  = static_cast<int*>(fill_area.get());
    auto* extra_base = static_cast<int*>(extra_area.get());

    for (size_t num_writes = config_.min_writes; num_writes <= config_.max_writes; num_writes += config_.writes_step) {
        size_t region_offset = (num_writes - 1) * region_size / kBytesPerEntry;

        int* fill_ptr           = fill_base + region_offset;
        volatile int* extra_ptr = extra_base + region_offset;

        writes_list.push_back(num_writes);

        WriteBufferResult res = measure_for_writes(num_writes, fill_ptr, extra_ptr, dummy, pmc.get());
        results.push_back(res);

        SPDLOG_DEBUG("| {:6} | {:12.2f} | {:6.2f} |", num_writes, res.avg_latency_ticks, res.latency_stddev);

        for (size_t i = 0; i < events.size(); ++i) {
            double per_sample = (res.avg_events.size() > i) ? double(res.avg_events[i]) / config_.iterations : 0.0;
            double total      = (res.avg_events.size() > i) ? double(res.avg_events[i]) : 0.0;
            SPDLOG_DEBUG("|        | {}: {:.2f} per sample (total {}) |", events[i], per_sample, total);
        }
    }

    if (results.size() >= 2) {
        size_t capacity = analyze_buffer_capacity(results, writes_list, sb_idx, bound_idx);
        SPDLOG_INFO("[{}] Estimated write buffer capacity: {} entries (each 4 bytes)", name(), capacity);
        data.write_buffer_size = capacity;
    }

    SPDLOG_INFO("[{}] write buffer measurement complete", name());
}

WriteBufferResult WriteBufferMeasurer::measure_for_writes(
    size_t num_writes,
    int* fill_base,
    volatile int* extra_addr,
    volatile int& dummy,
    platform::pmc::PmcGroup* pmc
) {
    // warmup
    for (size_t w = 0; w < config_.warmup_iterations; ++w) {
        for (size_t i = 0; i < num_writes; ++i) {
            fill_base[i * kStride] = static_cast<int>(i);
        }
        dummy = *extra_addr;
        platform::arch::lfence();
    }

    std::vector<double> time_samples;
    std::vector<std::vector<uint64_t>> pmc_samples;
    time_samples.reserve(config_.repeats);
    if (pmc)
        pmc_samples.reserve(config_.repeats);

    for (size_t r = 0; r < config_.repeats; ++r) {
        if (pmc) {
            pmc->reset();
            pmc->enable();
        }

        uint64_t total_ticks = 0;
        for (size_t iter = 0; iter < config_.iterations; ++iter) {
            // flush all write lines
            for (size_t i = 0; i < num_writes; ++i) {
                platform::arch::clflush(&fill_base[i * kStride]);
            }
            platform::arch::clflush(const_cast<void*>(reinterpret_cast<const volatile void*>(extra_addr)));
            platform::arch::flush_complete();

            // fill store buffer
            for (size_t i = 0; i < num_writes; ++i) {
                fill_base[i * kStride] = static_cast<int>(iter + i);
            }

            // measure critical store+load pair
            platform::arch::lfence();
            uint64_t start                  = platform::arch::tick();
            const_cast<int*>(extra_addr)[0] = 0xdeadbeef;
            dummy                           = *extra_addr;
            platform::arch::lfence();
            uint64_t end                    = platform::arch::tick();
            total_ticks += (end - start);
        }

        if (pmc) {
            pmc->disable();
            auto cv = pmc->read();
            if (cv.valid)
                pmc_samples.push_back(std::move(cv.values));
        }

        double avg_ticks = static_cast<double>(total_ticks) / config_.iterations;
        time_samples.push_back(avg_ticks);
    }

    double avg    = std::accumulate(time_samples.begin(), time_samples.end(), 0.0) / config_.repeats;
    double stddev = 0.0;
    for (double v : time_samples) {
        double d = v - avg;
        stddev += d * d;
    }
    stddev = std::sqrt(stddev / config_.repeats);

    std::vector<uint64_t> avg_events;
    if (pmc && !pmc_samples.empty()) {
        avg_events.assign(pmc_samples[0].size(), 0);
        for (const auto& sample : pmc_samples)
            for (size_t i = 0; i < sample.size(); ++i)
                avg_events[i] += sample[i];
        for (size_t i = 0; i < avg_events.size(); ++i)
            avg_events[i] /= config_.repeats;
    }
    if (pmc) {
        SPDLOG_DEBUG("[{}] num_writes={}, samples: ticks={:.2f}+-{:.2f}", name(), num_writes, avg, stddev);
        for (size_t i = 0; i < avg_events.size(); ++i) {
            SPDLOG_DEBUG(
                "[{}]   event{} = {} total, {:.2f} per iter",
                name(),
                i,
                avg_events[i],
                double(avg_events[i]) / config_.iterations
            );
        }
    }
    return {avg, stddev, std::move(avg_events)};
}

size_t WriteBufferMeasurer::analyze_buffer_capacity(
    const std::vector<WriteBufferResult>& results,
    const std::vector<size_t>& writes_list,
    size_t sb_idx,
    size_t bound_idx
) const {
    if (results.size() < config_.baseline_window + 2)
        return writes_list.back();

    // Baseline latency: median of first baseline_window points
    std::vector<double> base_samples;
    for (size_t i = 0; i < config_.baseline_window; ++i)
        base_samples.push_back(results[i].avg_latency_ticks);
    std::sort(base_samples.begin(), base_samples.end());
    double baseline = base_samples[base_samples.size() / 2];

    double spike_threshold = baseline * config_.latency_spike_ratio;
    double hold_threshold  = baseline * config_.latency_hold_ratio;

    size_t capacity = writes_list.back();

    // 1) Try to detect by latency spike
    for (size_t i = 1; i < results.size(); ++i) {
        if (results[i].avg_latency_ticks > spike_threshold) {
            size_t next_idx = i + 1;
            if (next_idx < results.size() && results[next_idx].avg_latency_ticks > hold_threshold) {
                capacity = writes_list[i];
                return capacity;
            }
        }
    }

    // Helper to get stall count per iteration
    auto getStalls = [&](const WriteBufferResult& res) -> double {
        if (sb_idx != std::string::npos && sb_idx < res.avg_events.size())
            return double(res.avg_events[sb_idx]) / config_.iterations;
        if (bound_idx != std::string::npos && bound_idx < res.avg_events.size())
            return double(res.avg_events[bound_idx]) / config_.iterations;
        return 0.0;
    };

    // 2) Fallback: detect by stall events using baseline and gradient

    // Baseline stalls from first baseline_window points
    double baseline_stalls = 0.0;
    size_t stall_window = std::min(config_.baseline_window, results.size());
    for (size_t i = 0; i < stall_window; ++i) {
        baseline_stalls += getStalls(results[i]);
    }
    baseline_stalls /= static_cast<double>(stall_window);

    // Absolute threshold to avoid noise when baseline_stalls is near zero
    double threshold_absolute = config_.stall_absolute_min;
    double threshold_relative = baseline_stalls * config_.stall_baseline_ratio;
    double stall_threshold = std::max(threshold_relative, threshold_absolute);

    // Find first point where stalls exceed threshold
    for (size_t i = 0; i < results.size(); ++i) {
        double stalls = getStalls(results[i]);
        if (stalls > stall_threshold) {
            capacity = writes_list[i];
            return capacity;
        }
    }

    // 3) Compare with median of recent points (relative threshold)
    const size_t window = config_.stall_median_window;
    if (results.size() > window) {
        for (size_t i = window; i < results.size(); ++i) {
            std::vector<double> recent;
            for (size_t j = i - window; j < i; ++j)
                recent.push_back(getStalls(results[j]));
            std::sort(recent.begin(), recent.end());
            double median = recent[recent.size() / 2];
            double cur = getStalls(results[i]);
            if (median > 0.0 && cur / median > config_.stall_gradient_ratio) {
                capacity = writes_list[i];
                return capacity;
            }
        }
    }

    SPDLOG_DEBUG(
        "[{}] baseline = {:.2f}, threshold = {:.2f}, capacity = {}",
        name(),
        baseline,
        spike_threshold,
        capacity
    );
    return capacity;
}

}  // namespace silicon_probe::write_buffer
