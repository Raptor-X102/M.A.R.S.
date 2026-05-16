// measurement/return_address_stack/return_address_stack_measurer.cpp
#include "measurement/return_address_stack/return_address_stack_measurer.hpp"
#include "measurement/common/statistics.hpp"

#include <algorithm>

namespace silicon_probe::return_address_stack {

namespace statistics = silicon_probe::common::statistics;

ReturnAddressStackMeasurer::ReturnAddressStackMeasurer() : ReturnAddressStackMeasurer(Config{}) {}

ReturnAddressStackMeasurer::ReturnAddressStackMeasurer(Config config) : config_(std::move(config)) {
    validateConfig();
    SPDLOG_DEBUG(
        "[{}] configured: min_recursion_depth={}, max_recursion_depth={}, recursion_depth_step={}, iterations={}",
        name(),
        config_.min_recursion_depth,
        config_.max_recursion_depth,
        config_.recursion_depth_step,
        config_.iterations
    );
}

std::string_view ReturnAddressStackMeasurer::name() const noexcept { return "return address stack"; }

void ReturnAddressStackMeasurer::validateConfig() {
    if (config_.min_recursion_depth == 0)
        config_.min_recursion_depth = kDefaultMinRecursion;
    if (config_.max_recursion_depth < config_.min_recursion_depth)
        config_.max_recursion_depth = config_.min_recursion_depth;
    if (config_.max_recursion_depth > kMaxSafeRecursionDepth) {
        SPDLOG_WARN("[{}] max_recursion_depth {} exceeds safe limit {}, clamping",
                    name(), config_.max_recursion_depth, kMaxSafeRecursionDepth);
        config_.max_recursion_depth = kMaxSafeRecursionDepth;
    }
    if (config_.recursion_depth_step == 0)
        config_.recursion_depth_step = kDefaultRecursionStep;
    constexpr size_t kMaxIter = 1'000'000;
    if (config_.iterations == 0)
        config_.iterations = kDefaultIterations;
    if (config_.iterations > kMaxIter) {
        SPDLOG_WARN("[{}] iterations {} too high, reducing to {}",
                    name(), config_.iterations, kMaxIter);
        config_.iterations = kMaxIter;
    }
    if (config_.trim_ratio < 0.0)
        config_.trim_ratio = 0.0;
    if (config_.trim_ratio >= 0.5) {
        SPDLOG_WARN("[{}] trim_ratio {} >= 0.5, setting to 0.49", name(), config_.trim_ratio);
        config_.trim_ratio = 0.49;
    }
    if (config_.smoothing_window == 0)
        config_.smoothing_window = 3;
    if (config_.smoothing_window % 2 == 0)
        config_.smoothing_window++;
    if (config_.noise_estimation_ratio <= 0.0 || config_.noise_estimation_ratio > 1.0)
        config_.noise_estimation_ratio = 0.5;
    if (config_.threshold_multiplier <= 0.0)
        config_.threshold_multiplier = 5.0;
    if (config_.sustained_window == 0)
        config_.sustained_window = 3;
    if (config_.sustained_ratio <= 1.0) {
        SPDLOG_WARN("[{}] sustained_ratio {} <= 1, forcing to 1.15", name(), config_.sustained_ratio);
        config_.sustained_ratio = 1.15;
    }
}

__attribute__((noinline, noclone, noipa)) void ReturnAddressStackMeasurer::recursive_func(size_t depth, size_t iteration) {
    if (iteration >= depth)
        return;
    recursive_func(depth, iteration + 1);
}

void ReturnAddressStackMeasurer::measure(shared_types::CpuInfoData& data) {
    SPDLOG_INFO("[{}] starting Return Address Stack size measurement", name());

    platform::ScopedMeasurementEnvironment environment{config_.environment};

    std::vector<Result> results;

    for (size_t depth = config_.min_recursion_depth; depth <= config_.max_recursion_depth;
         depth += config_.recursion_depth_step) {
        std::vector<uint64_t> raw_exec_times;
        raw_exec_times.reserve(config_.iterations);

        for (size_t outer = 0; outer < config_.iterations; ++outer) {
            uint64_t start = platform::arch::tick();
            recursive_func(depth, 0);
            uint64_t end = platform::arch::tick();
            raw_exec_times.push_back(end - start);
        }

        // Sort to filter outliers
        std::sort(raw_exec_times.begin(), raw_exec_times.end());

        // Trim outliers from both ends
        size_t trim_count = static_cast<size_t>(config_.iterations * config_.trim_ratio);
        if (trim_count * 2 < raw_exec_times.size()) {
            raw_exec_times.erase(raw_exec_times.begin(), raw_exec_times.begin() + trim_count);
            raw_exec_times.erase(raw_exec_times.end() - trim_count, raw_exec_times.end());
        }

        // Ensure we have at least one sample after trimming
        if (raw_exec_times.empty()) {
            SPDLOG_WARN("[{}] No samples left after trimming for depth={}", name(), depth);
            continue;
        }

        uint64_t min_time = raw_exec_times.front();
        uint64_t max_time = raw_exec_times.back();
        double avg_time = statistics::mean(raw_exec_times);

        results.push_back({depth, avg_time});

        SPDLOG_DEBUG(
            "[{}] depth={:3d}  min={:3d}  avg={:6.2f}  max={:3d}",
            name(),
            depth,
            min_time,
            avg_time,
            max_time
        );
    }

    if (results.empty()) {
        SPDLOG_ERROR("[{}] no valid data collected", name());
        return;
    }

    int ras_size = detectRASSaturation(results);
    if (ras_size > 0) {
        data.ras_size = ras_size;
        SPDLOG_INFO("[{}] Return Address Stack effective size ≈ {} addresses", name(), ras_size);
    } else {
        SPDLOG_ERROR(
            "[{}] could not detect RAS saturation in period range [{}, {}]",
            name(),
            config_.min_recursion_depth,
            config_.max_recursion_depth
        );
    }

    SPDLOG_INFO("[{}] measurement complete", name());
}

int ReturnAddressStackMeasurer::detectRASSaturation(const std::vector<Result>& results) const {
    auto results_size = results.size();
    if (results_size < 2)  // достаточно двух точек для поиска скачка
        return -1;

    // ----- 1. Median smoothing -----
    size_t win = config_.smoothing_window;
    if (win % 2 == 0) ++win;
    int half = static_cast<int>(win / 2);
    std::vector<double> raw(results_size);
    for (size_t i = 0; i < results_size; ++i)
        raw[i] = results[i].avg_exec_time;

    std::vector<double> smoothed(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        int left  = static_cast<int>(i) - half;
        int right = static_cast<int>(i) + half;
        if (left < 0) left = 0;
        if (right >= static_cast<int>(raw.size())) right = static_cast<int>(raw.size()) - 1;
        std::vector<double> window;
        for (int j = left; j <= right; ++j) window.push_back(raw[j]);
        smoothed[i] = statistics::compute_median(std::move(window));
    }

    // ----- 2. Find depth where average time after is significantly higher than before -----
    const size_t n = smoothed.size();
    if (n < 2) return -1;

    int best_idx = -1;
    double best_ratio = 1.0;

    for (size_t i = 0; i < n - 1; ++i) {
        double sum_before = 0.0;
        for (size_t j = 0; j <= i; ++j)
            sum_before += smoothed[j];
        double avg_before = sum_before / (i + 1);

        double sum_after = 0.0;
        for (size_t j = i + 1; j < n; ++j)
            sum_after += smoothed[j];
        double avg_after = sum_after / (n - i - 1);

        if (avg_before > 0) {
            double ratio = avg_after / avg_before;
            if (ratio > best_ratio) {
                best_ratio = ratio;
                best_idx = static_cast<int>(i);
            }
        }
    }

    if (best_idx >= 0 && best_ratio >= config_.sustained_ratio) {
        return static_cast<int>(results[best_idx].depth);
    }

    return -1;
}

}  // namespace silicon_probe::return_address_stack
