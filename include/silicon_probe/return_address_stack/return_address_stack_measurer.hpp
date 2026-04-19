// branch_history_table_measurer.hpp
#pragma once

#include "silicon_probe/infra/logging.hpp"
#include "silicon_probe/core/measurer.hpp"
#include "silicon_probe/platform/arch.hpp"
#include "silicon_probe/platform/cpu_vendor.hpp"
#include "silicon_probe/platform/os.hpp"

#include <vector>
#include <random>
#include <cmath>
#include <algorithm>

namespace silicon_probe::return_address_stack {

class ReturnAddressStackMeasurer final : public core::Measurer {
public:
    static constexpr size_t kDefaultMinRecursion = 1;
    static constexpr size_t kDefaultMaxRecursion  = 32;
    static constexpr size_t kDefaultRecursionStep = 1;
    static constexpr size_t kDefaultIterations = 100'000'000;

    struct Config {
        platform::MeasurementEnvironmentOptions environment;
        size_t min_recursion_depth = kDefaultMinRecursion;
        size_t max_recursion_depth = kDefaultMaxRecursion;
        size_t recursion_depth_step = kDefaultRecursionStep;
        size_t iterations = kDefaultIterations;
    };

private:
    Config config_;

    struct Result {
        size_t depth;
        size_t min_exec_time;
        double avg_exec_time;
        size_t max_exec_time;
    };

public:
    ReturnAddressStackMeasurer() : ReturnAddressStackMeasurer(Config{}) {}
    explicit ReturnAddressStackMeasurer(Config config) : config_(std::move(config)) {
        SPDLOG_INFO("[{}] configured: min_recursion_depth={}, max_recursion_depth={}, recursion_depth_step={}, iterations={}",
                    name(), config_.min_recursion_depth, config_.max_recursion_depth,
                    config_.recursion_depth_step, config_.iterations);
    }

    std::string_view name() const noexcept override { return "return address stack"; }

    void measure(core::CpuInfoData& data) override {
        SPDLOG_INFO("[{}] starting Return Address Stack size measurement", name());

        platform::ScopedMeasurementEnvironment environment{config_.environment};

        std::vector<Result> results;

        for (size_t depth = config_.min_recursion_depth; depth <= config_.max_recursion_depth; depth += config_.recursion_depth_step) {

            std::vector<uint64_t> raw_exec_times;
            raw_exec_times.reserve(config_.iterations);

            for (size_t outer = 0; outer < config_.iterations; ++outer) {
                //volatile int sink = 0;
                uint64_t start = platform::arch::tick();
                recursive_func(depth, 0/*, sink*/);
                uint64_t end = platform::arch::tick();
                raw_exec_times.push_back(end - start);
            }

            // Sort to filter outliers
            std::sort(raw_exec_times.begin(), raw_exec_times.end());

            // Trim 2% from each end (keep 96% of samples)
            size_t trim_count = static_cast<size_t>(config_.iterations * 0.02);
            if (trim_count * 2 < raw_exec_times.size()) {
                raw_exec_times.erase(raw_exec_times.begin(), raw_exec_times.begin() + trim_count);
                raw_exec_times.erase(raw_exec_times.end() - trim_count, raw_exec_times.end());
            }

            uint64_t min_time = raw_exec_times.front();
            uint64_t max_time = raw_exec_times.back();
            uint64_t sum_time = std::accumulate(raw_exec_times.begin(), raw_exec_times.end(), 0ULL);
            double avg_time = static_cast<double>(sum_time) / raw_exec_times.size();

            results.push_back({depth, min_time, avg_time, max_time});

            SPDLOG_INFO("[{}] depth={:3d}  min={:3d}  avg={:6.2f}  max={:3d}",
                        name(), depth, min_time, avg_time, max_time);
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
            SPDLOG_ERROR("[{}] could not detect RAS saturation in period range [{}, {}]",
                         name(), config_.min_recursion_depth, config_.max_recursion_depth);
        }

        SPDLOG_INFO("[{}] measurement complete", name());
    }

private:
    __attribute__((noinline, noclone))
    void recursive_func(size_t depth, size_t iteration/*, volatile int& sink*/) {
        if (iteration >= depth)
            return;

        recursive_func(depth, iteration+1/*, sink*/);
        //++sink;
    }

 //   __attribute__((noinline, noclone))
 //   void recursive_func(size_t depth, size_t iteration, volatile int& sink) {
 //       if (iteration >= depth)
 //           return;

 //       recursive_func(depth, iteration+1, sink);
 //       ++sink;
 //   }

    int detectRASSaturation(const std::vector<Result>& results) const {
        if (results.size() < 16) return -1;
        std::vector<double> deltas;
        for (size_t i = 1; i < results.size(); ++i)
            deltas.push_back(results[i].avg_exec_time - results[i-1].avg_exec_time);
        
        // Baseline: median of deltas for depths 8..16 (indices 7..15)
        std::vector<double> baseline_vals(deltas.begin() + 7, deltas.begin() + std::min(16, (int)deltas.size()));
        std::sort(baseline_vals.begin(), baseline_vals.end());
        double baseline = baseline_vals[baseline_vals.size()/2];
        const double threshold = baseline * 1.5;
        
        // Find first depth where delta > threshold and next 2 deltas also > threshold
        for (size_t i = 8; i < deltas.size() - 2; ++i) {
            if (deltas[i] > threshold && deltas[i+1] > threshold && deltas[i+2] > threshold) {
                return results[i].depth;  // depth at which delta occurred
            }
        }
        return -1;
    }
};

} // namespace silicon_probe::branch_history_table
