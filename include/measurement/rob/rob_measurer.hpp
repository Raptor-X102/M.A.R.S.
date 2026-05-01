// rob_measurer.hpp
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <numeric>
#include <random>
#include <vector>

#include "core/measurer.hpp"
#include "infra/logging.hpp"
#include "platform/arch.hpp"
#include "platform/os.hpp"
#include "platform/pmc.hpp"

namespace silicon_probe::rob {

class RobMeasurer final : public core::Measurer {
   public:
    static constexpr size_t kDefaultMinInstrCnt = 50;
    static constexpr size_t kDefaultMaxInstrCnt = 600;
    static constexpr size_t kDefaultInstrCntStep = 10;
    static constexpr size_t kDefaultWarmupIterations = 2;
    static constexpr size_t kDefaultInnerIterations = 8192;
    static constexpr size_t kDefaultOuterIterations = 64;
    static constexpr size_t kDefaultUnroll = 17;
    static constexpr int kDefaultInstrType = 4;

    struct Config {
        bool enabled = true;
        platform::MeasurementEnvironmentOptions environment;
        size_t min_instr_cnt = kDefaultMinInstrCnt;
        size_t max_instr_cnt = kDefaultMaxInstrCnt;
        size_t instr_cnt_step = kDefaultInstrCntStep;
        size_t warmup_iterations = kDefaultWarmupIterations;
        size_t inner_iterations = kDefaultInnerIterations;
        size_t outer_iterations = kDefaultOuterIterations;
        int instr_type = kDefaultInstrType;
        double baseline_fraction = 0.2;
        size_t baseline_min_samples = 5;
        size_t required_consecutive_points = 3;
        double saturation_threshold_ratio = 1.15;
        double fallback_jump_ratio = 0.5;
    };

   private:
    Config config_;

    struct Result {
        size_t filler;
        double min_cycles_per_iter;
        double avg_cycles_per_iter;
        double max_cycles_per_iter;
    };

   public:
    RobMeasurer() : RobMeasurer(Config{}) {}
    explicit RobMeasurer(Config config) : config_(std::move(config)) {
        SPDLOG_INFO("[{}] configured: min={}, max={}, step={}, inner_its={}, outer_its={}, instr_type={}", name(),
                    config_.min_instr_cnt, config_.max_instr_cnt, config_.instr_cnt_step, config_.inner_iterations,
                    config_.outer_iterations, config_.instr_type);
    }

    std::string_view name() const noexcept override { return "rob"; }

    void measure(shared_types::CpuInfoData& data) override {
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

   private:
    int detectRobSaturation(const std::vector<Result>& results) {
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
};

}  // namespace silicon_probe::rob
