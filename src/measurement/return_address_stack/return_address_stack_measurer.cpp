// measurement/return_address_stack/return_address_stack_measurer.cpp
#include "measurement/return_address_stack/return_address_stack_measurer.hpp"

#include <numeric>

namespace silicon_probe::return_address_stack {

ReturnAddressStackMeasurer::ReturnAddressStackMeasurer() : ReturnAddressStackMeasurer(Config{}) {}

ReturnAddressStackMeasurer::ReturnAddressStackMeasurer(Config config) : config_(std::move(config)) {
    SPDLOG_INFO(
        "[{}] configured: min_recursion_depth={}, max_recursion_depth={}, recursion_depth_step={}, iterations={}",
        name(), config_.min_recursion_depth, config_.max_recursion_depth, config_.recursion_depth_step,
        config_.iterations
    );
}

std::string_view ReturnAddressStackMeasurer::name() const noexcept { return "return address stack"; }

__attribute__((noinline, noclone)) void ReturnAddressStackMeasurer::recursive_func(size_t depth, size_t iteration) {
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

        // Trim 2% from each end (keep 96% of samples)
        size_t trim_count = static_cast<size_t>(config_.iterations * config_.trim_ratio);
        if (trim_count * 2 < raw_exec_times.size()) {
            raw_exec_times.erase(raw_exec_times.begin(), raw_exec_times.begin() + trim_count);
            raw_exec_times.erase(raw_exec_times.end() - trim_count, raw_exec_times.end());
        }

        uint64_t min_time = raw_exec_times.front();
        uint64_t max_time = raw_exec_times.back();
        uint64_t sum_time = std::accumulate(raw_exec_times.begin(), raw_exec_times.end(), 0ULL);
        double avg_time   = static_cast<double>(sum_time) / raw_exec_times.size();

        results.push_back({depth, min_time, avg_time, max_time});

        SPDLOG_INFO("[{}] depth={:3d}  min={:3d}  avg={:6.2f}  max={:3d}", name(), depth, min_time, avg_time, max_time);
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
            "[{}] could not detect RAS saturation in period range [{}, {}]", name(), config_.min_recursion_depth,
            config_.max_recursion_depth
        );
    }

    SPDLOG_INFO("[{}] measurement complete", name());
}

int ReturnAddressStackMeasurer::detectRASSaturation(const std::vector<Result>& results) const {
    // Need enough points for meaningful detection
    if (results.size() < config_.sustained_window + 2)
        return -1;

    // ----- 1. Median smoothing -----
    size_t win = config_.smoothing_window;
    if (win % 2 == 0)
        ++win;  // ensure odd
    int half = static_cast<int>(win / 2);
    std::vector<double> raw(results.size());
    for (size_t i = 0; i < results.size(); ++i)
        raw[i] = results[i].avg_exec_time;

    std::vector<double> smoothed(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        int left  = static_cast<int>(i) - half;
        int right = static_cast<int>(i) + half;
        if (left < 0)
            left = 0;
        if (right >= static_cast<int>(raw.size()))
            right = static_cast<int>(raw.size()) - 1;
        std::vector<double> window;
        for (int j = left; j <= right; ++j)
            window.push_back(raw[j]);
        std::sort(window.begin(), window.end());
        smoothed[i] = window[window.size() / 2];
    }

    // ----- 2. Deltas -----
    std::vector<double> deltas;
    deltas.reserve(smoothed.size() - 1);
    for (size_t i = 1; i < smoothed.size(); ++i)
        deltas.push_back(smoothed[i] - smoothed[i - 1]);

    if (deltas.size() < config_.sustained_window + 2)
        return -1;

    // ----- 3. Noise estimation on first 'ratio' fraction of deltas -----
    size_t noise_len = static_cast<size_t>(deltas.size() * config_.noise_estimation_ratio);
    if (noise_len < 2)
        noise_len = deltas.size();
    std::vector<double> noise(deltas.begin(), deltas.begin() + noise_len);
    std::sort(noise.begin(), noise.end());
    double median = noise[noise.size() / 2];
    std::vector<double> abs_dev;
    abs_dev.reserve(noise.size());
    for (double d : noise)
        abs_dev.push_back(std::abs(d - median));
    std::sort(abs_dev.begin(), abs_dev.end());
    double mad       = abs_dev[abs_dev.size() / 2];
    double threshold = median + config_.threshold_multiplier * mad;

    // ----- 4. Find first jump where average level after stays significantly higher -----
    const size_t W = config_.sustained_window;
    for (size_t i = 0; i + W < deltas.size(); ++i) {
        if (deltas[i] > threshold) {
            // Average of W points after the jump (starting from depth i+1)
            double after_sum = 0.0;
            for (size_t j = 1; j <= W; ++j)
                after_sum += smoothed[i + j];
            double after_avg = after_sum / W;
            // Average of W points before the jump (ending at depth i)
            double before_sum = 0.0;
            for (size_t j = 0; j < W; ++j)
                before_sum += smoothed[i - j];
            double before_avg = before_sum / W;
            if (after_avg >= before_avg * config_.sustained_ratio) {
                // results[i].depth is the depth before the jump (correct RAS size)
                return static_cast<int>(results[i].depth);
            }
        }
    }
    return -1;
}

}  // namespace silicon_probe::return_address_stack
