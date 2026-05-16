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
    if (results.size() < kMinResultsCnt) return -1;

    std::vector<double> values;
    values.reserve(results.size());
    for (const auto& r : results) values.push_back(r.avg_cycles_per_iter);

    // ---- Step 1: find maximum relative jump ----
    size_t max_jump_idx = 0;
    double max_rel_jump = 0.0;
    for (size_t i = 1; i < results.size(); ++i) {
        double prev = values[i-1];
        double curr = values[i];
        if (prev == 0.0) continue;
        double rel_jump = (curr - prev) / prev;
        if (rel_jump > max_rel_jump) {
            max_rel_jump = rel_jump;
            max_jump_idx = i;
        }
    }

    SPDLOG_DEBUG("[{}] max_rel_jump={:.3f} at filler={}", 
                 name(), max_rel_jump, results[max_jump_idx].filler);

    if (max_rel_jump > config_.fallback_jump_ratio) {
        bool sustained = true;
        size_t verify_cnt = std::min<size_t>(config_.required_consecutive_points, 
                                             results.size() - max_jump_idx);
        if (verify_cnt > 0) {
            double first_after = values[max_jump_idx];
            double drop_tolerance = config_.sustain_threshold;   // вместо 0.9
            for (size_t k = 1; k < verify_cnt; ++k) {
                if (values[max_jump_idx + k] < first_after * drop_tolerance) {
                    sustained = false;
                    SPDLOG_DEBUG("[{}] jump not sustained: value at filler={} dropped below {:.2f}",
                                 name(), results[max_jump_idx + k].filler, first_after * drop_tolerance);
                    break;
                }
            }
        }
        if (sustained) {
            int rob = static_cast<int>(results[max_jump_idx].filler) + 1;
            SPDLOG_DEBUG("[{}] using jump method, ROB={}", name(), rob);
            return rob;
        } else {
            SPDLOG_DEBUG("[{}] jump discarded, falling back to threshold method", name());
        }
    }

    // ---- Step 2: fallback to threshold-based consecutive detection ----
    size_t baseline_cnt = std::max<size_t>(
        config_.baseline_min_samples,
        static_cast<size_t>(std::ceil(static_cast<double>(results.size()) * config_.baseline_fraction))
    );
    baseline_cnt = std::min(baseline_cnt, results.size());

    std::vector<double> baseline_vals(values.begin(), values.begin() + baseline_cnt);
    double baseline = silicon_probe::common::statistics::compute_median(baseline_vals);
    double threshold = baseline * config_.saturation_threshold_ratio;

    size_t consecutive = 0;
    size_t jump_idx = results.size();

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
        return static_cast<int>(results[jump_idx].filler) + 1;
    }

    return -1;
}

}  // namespace silicon_probe::rob
