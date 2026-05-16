#include "measurement/rob/rob_measurer.hpp"
#include "measurement/common/statistics.hpp" 
#include <algorithm>
#include <cmath>
#include <numeric>

namespace silicon_probe::rob {

namespace statistics = silicon_probe::common::statistics;

RobMeasurer::RobMeasurer() : RobMeasurer(Config{}) {}

RobMeasurer::RobMeasurer(Config config) : config_(std::move(config)) {
    validateConfig();  
    SPDLOG_DEBUG(
        "[{}] configured: min={}, max={}, step={}, inner_its={}, outer_its={}, instr_type={}, unroll={}",
        name(),
        config_.min_instr_cnt,
        config_.max_instr_cnt,
        config_.instr_cnt_step,
        config_.inner_iterations,
        config_.outer_iterations,
        config_.instr_type,
        config_.unroll
    );
}

void RobMeasurer::validateConfig() {
    // Check and fix min_instr_cnt / max_instr_cnt
    if (config_.min_instr_cnt == 0) {
        SPDLOG_WARN("[{}] min_instr_cnt cannot be 0, reset to default {}", name(), kDefaultMinInstrCnt);
        config_.min_instr_cnt = kDefaultMinInstrCnt;
    }
    if (config_.max_instr_cnt < config_.min_instr_cnt) {
        SPDLOG_WARN("[{}] max_instr_cnt < min_instr_cnt, swapping", name());
        std::swap(config_.min_instr_cnt, config_.max_instr_cnt);
    }
    if (config_.instr_cnt_step == 0) {
        SPDLOG_WARN("[{}] instr_cnt_step cannot be 0, reset to default {}", name(), kDefaultInstrCntStep);
        config_.instr_cnt_step = kDefaultInstrCntStep;
    }
    if (config_.inner_iterations == 0) {
        SPDLOG_WARN("[{}] inner_iterations cannot be 0, reset to default {}", name(), kDefaultInnerIterations);
        config_.inner_iterations = kDefaultInnerIterations;
    }
    if (config_.outer_iterations == 0) {
        SPDLOG_WARN("[{}] outer_iterations cannot be 0, reset to default {}", name(), kDefaultOuterIterations);
        config_.outer_iterations = kDefaultOuterIterations;
    }
    if (config_.unroll == 0) {
        SPDLOG_WARN("[{}] unroll cannot be 0, reset to default {}", name(), kDefaultUnroll);
        config_.unroll = kDefaultUnroll;
    }
    if (config_.instr_type < 0) {
        SPDLOG_WARN("[{}] negative instr_type not allowed, reset to default {}", name(), kDefaultInstrType);
        config_.instr_type = kDefaultInstrType;
    }
    if (config_.baseline_fraction <= 0.0 || config_.baseline_fraction > 1.0) {
        SPDLOG_WARN("[{}] baseline_fraction out of (0,1], reset to 0.2", name());
        config_.baseline_fraction = 0.2;
    }
    if (config_.baseline_min_samples == 0) {
        config_.baseline_min_samples = 5;
    }
    if (config_.required_consecutive_points == 0) {
        config_.required_consecutive_points = 3;
    }
    if (config_.saturation_threshold_ratio < 1.0) {
        SPDLOG_WARN("[{}] saturation_threshold_ratio must be >=1.0, reset to 1.15", name());
        config_.saturation_threshold_ratio = 1.15;
    }
    if (config_.fallback_jump_ratio <= 0.0) {
        SPDLOG_WARN("[{}] fallback_jump_ratio must be positive, reset to 0.5", name());
        config_.fallback_jump_ratio = 0.5;
    }
}

std::string_view RobMeasurer::name() const noexcept { return "rob"; }

void RobMeasurer::measure(shared_types::CpuInfoData& data) {
    if (!config_.enabled) {
        SPDLOG_DEBUG("[{}] measurement disabled", name());
        return;
    }

    SPDLOG_INFO("[{}] starting ROB measurement (Wong method)", name());
    platform::ScopedMeasurementEnvironment environment{config_.environment};

    platform::arch::set_rob_inner_iterations(config_.inner_iterations);

    using FuncPtr = void (*)();

    double factor = 0.5 / (static_cast<double>(config_.inner_iterations) * config_.unroll);

    std::vector<Result> results;
    results.reserve((config_.max_instr_cnt - config_.min_instr_cnt) / config_.instr_cnt_step + 1);

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

        auto [min_it, max_it] = std::minmax_element(raw_cycles.begin(), raw_cycles.end());
        uint64_t min_cycles = *min_it;
        uint64_t max_cycles = *max_it;
        double avg_cycles = statistics::mean(raw_cycles);

        double min_per_iter = factor * min_cycles;
        double avg_per_iter = factor * avg_cycles;
        double max_per_iter = factor * max_cycles;

        results.push_back({filler, min_per_iter, avg_per_iter, max_per_iter});

        SPDLOG_DEBUG(
            "[{}] filler={:3d}  min={:6.2f}  avg={:6.2f}  max={:6.2f}",
            name(),
            filler,
            min_per_iter,
            avg_per_iter,
            max_per_iter
        );

        platform::arch::release_rob_code();
    }

    if (results.empty()) {
        SPDLOG_ERROR("[{}] no valid data collected", name());
        return;
    }

    int rob_size = detectRobSaturation(results);
    if (rob_size < 0) {
        SPDLOG_ERROR(
            "[{}] could not detect ROB saturation in range [{}, {}]",
            name(),
            config_.min_instr_cnt,
            config_.max_instr_cnt
        );
    } else {
        SPDLOG_INFO("[{}] ROB size estimated: {} entries", name(), rob_size);
        data.rob_size = rob_size;
    }

    SPDLOG_INFO("[{}] ROB measurement complete", name());
}

int RobMeasurer::detectRobSaturation(const std::vector<Result>& results) {
    if (results.size() < 10) return -1;

    std::vector<double> values;
    values.reserve(results.size());
    for (const auto& r : results) {
        values.push_back(r.avg_cycles_per_iter);
    }

    // Compute baseline median from first baseline_fraction points
    size_t baseline_cnt = std::max<size_t>(
        config_.baseline_min_samples,
        static_cast<size_t>(std::ceil(static_cast<double>(results.size()) * config_.baseline_fraction))
    );
    baseline_cnt = std::min(baseline_cnt, results.size());

    std::vector<double> baseline_vals(values.begin(), values.begin() + baseline_cnt);
    double baseline = statistics::compute_median(baseline_vals);

    double threshold = baseline * config_.saturation_threshold_ratio;
    size_t consecutive = 0;
    size_t jump_idx = results.size();

    // Linear scan for required_consecutive_points consecutive values above threshold
    for (size_t i = baseline_cnt; i < results.size(); ++i) {
        if (values[i] > threshold) {
            ++consecutive;
            if (consecutive >= config_.required_consecutive_points) {
                jump_idx = i - config_.required_consecutive_points + 1;
                break;
            }
        } else {
            consecutive = 0;
        }
    }

    if (jump_idx < results.size()) {
        // ROB size = filler value at jump point + 1
        return static_cast<int>(results[jump_idx].filler) + 1;
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
    if (max_diff > baseline * config_.fallback_jump_ratio) {
        return static_cast<int>(results[max_diff_idx + 1].filler) + 1;
    }

    return -1;
}

}  // namespace silicon_probe::rob
