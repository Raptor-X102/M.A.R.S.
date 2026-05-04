// measurement/rob/rob_measurer.cpp
#include "measurement/rob/rob_measurer.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace silicon_probe::rob {

RobMeasurer::RobMeasurer() : RobMeasurer(Config{}) {}

RobMeasurer::RobMeasurer(Config config) : config_(std::move(config)) {
    SPDLOG_INFO("[{}] configured: min={}, max={}, step={}, inner_its={}, outer_its={}, instr_type={}", name(),
                config_.min_instr_cnt, config_.max_instr_cnt, config_.instr_cnt_step, config_.inner_iterations,
                config_.outer_iterations, config_.instr_type);
}

std::string_view RobMeasurer::name() const noexcept {
    return "rob";
}

void RobMeasurer::measure(shared_types::CpuInfoData& data) {
    SPDLOG_INFO("[{}] starting ROB measurement (Wong method)", name());
    platform::ScopedMeasurementEnvironment environment{config_.environment};

    platform::arch::set_rob_inner_iterations(config_.inner_iterations);

    using FuncPtr = void (*)();

    std::vector<Result> results;

    for (size_t filler = config_.min_instr_cnt; filler <= config_.max_instr_cnt; filler += config_.instr_cnt_step) {
        FuncPtr fn = reinterpret_cast<FuncPtr>(platform::arch::generate_rob_code(filler, config_.instr_type));
        if (!fn) {
            SPDLOG_WARN("[{}] JIT failed for filler={}", name(), filler);
            continue;
        }

        // Warmup
        for (size_t w = 0; w < config_.warmup_iterations; ++w) {
            fn();
        }

        std::vector<uint64_t> raw_cycles;
        raw_cycles.reserve(config_.outer_iterations);

        for (size_t outer = 0; outer < config_.outer_iterations; ++outer) {
            uint64_t start = platform::arch::tick();
            fn();
            uint64_t end = platform::arch::tick();
            raw_cycles.push_back(end - start);
        }

        uint64_t min_cycles = raw_cycles[0];
        uint64_t max_cycles = raw_cycles[0];
        uint64_t sum_cycles = 0;
        for (auto c : raw_cycles) {
            if (c < min_cycles) min_cycles = c;
            if (c > max_cycles) max_cycles = c;
            sum_cycles += c;
        }
        double avg_cycles = static_cast<double>(sum_cycles) / config_.outer_iterations;

        double factor = 0.5 / (config_.inner_iterations * kDefaultUnroll);
        double min_per_iter = factor * min_cycles;
        double avg_per_iter = factor * avg_cycles;
        double max_per_iter = factor * max_cycles;

        results.push_back({filler, min_per_iter, avg_per_iter, max_per_iter});

        SPDLOG_INFO("[{}] filler={:3d}  min={:6.2f}  avg={:6.2f}  max={:6.2f}", name(), filler, min_per_iter,
                    avg_per_iter, max_per_iter);

        platform::arch::release_rob_code();
    }

    if (results.empty()) {
        SPDLOG_ERROR("[{}] no valid data collected", name());
        return;
    }

    int rob_size = detectRobSaturation(results);
    if (rob_size < 0) {
        SPDLOG_ERROR("[{}] could not detect ROB saturation in range [{}, {}]", name(), config_.min_instr_cnt,
                     config_.max_instr_cnt);
    } else {
        SPDLOG_INFO("[{}] ROB size estimated: {} entries", name(), rob_size);
        data.rob_size = rob_size;
    }

    platform::arch::release_rob_code();
    SPDLOG_INFO("[{}] ROB measurement complete", name());
}

int RobMeasurer::detectRobSaturation(const std::vector<Result>& results) {
    if (results.size() < 10) return -1;

    // Use average cycles per iteration (more stable than min)
    std::vector<double> values;
    for (const auto& r : results) {
        values.push_back(r.avg_cycles_per_iter);
    }

    // Compute baseline median from first 20% of points (low filler range)
    size_t baseline_cnt = std::max<size_t>(
        config_.baseline_min_samples,
        static_cast<size_t>(std::ceil(static_cast<double>(results.size()) * config_.baseline_fraction)));
    baseline_cnt = std::min(baseline_cnt, results.size());
    std::vector<double> baseline_vals(values.begin(), values.begin() + baseline_cnt);
    std::sort(baseline_vals.begin(), baseline_vals.end());
    double baseline = baseline_vals[baseline_vals.size() / 2];

    // Find first index where value exceeds baseline by at least 15%
    double threshold = baseline * config_.saturation_threshold_ratio;
    size_t rob_idx = results.size();

    for (size_t i = baseline_cnt; i < results.size(); ++i) {
        if (values[i] > threshold) {
            // Require at least 3 consecutive points above threshold
            bool stable = true;
            for (size_t j = i; j < std::min(i + config_.required_consecutive_points, results.size()); ++j) {
                if (values[j] <= threshold) {
                    stable = false;
                    break;
                }
            }
            if (stable) {
                rob_idx = i;
                break;
            }
        }
    }

    if (rob_idx < results.size()) {
        // ROB size corresponds to filler value at jump point
        // In Wong's method, ROB size = icount = filler + 1
        int rob_entries = static_cast<int>(results[rob_idx].filler) + 1;
        return rob_entries;
    }

    // Fallback: detect maximum consecutive difference (for clear jumps)
    double max_diff = 0.0;
    size_t max_diff_idx = 0;
    for (size_t i = baseline_cnt; i < results.size() - 1; ++i) {
        double diff = values[i + 1] - values[i];
        if (diff > max_diff) {
            max_diff = diff;
            max_diff_idx = i;
        }
    }
    if (max_diff > baseline * config_.fallback_jump_ratio) {  // significant jump
        return static_cast<int>(results[max_diff_idx + 1].filler) + 1;
    }

    return -1;
}

}  // namespace silicon_probe::rob
