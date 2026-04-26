#pragma once

#include "infra/logging.hpp"
#include "measurement/core/measurer.hpp"
#include "platform/arch.hpp"
#include "platform/pmc.hpp"
#include "platform/os.hpp"
#include "platform/events_discovery.hpp"
#include <vector>
#include <string>
#include <algorithm>
#include <numeric>
#include <cmath>

namespace silicon_probe::store_to_load_forwarding {

struct StoreToLoadForwardingResult {
    double avg_ticks_per_block;
    double avg_ticks_per_iter;
    double ticks_std;
    std::vector<uint64_t> avg_events_counts;
};

class StoreToLoadForwardingMeasurer final : public core::Measurer {
  public:
    static constexpr size_t kDefaultAlignment = 16;
    static constexpr size_t kDefaultBufferSize = 2*64 / sizeof(size_t); // 2 cache lines in size_t
    static constexpr size_t kDefaultMinOffest = 0;
    static constexpr size_t kDefaultMaxOffest = kDefaultBufferSize - 2;
    static constexpr size_t kDefaultOffsetStep = 1;
    static constexpr size_t kDefaultIterations = 100'000'000;
    static constexpr size_t kDefaultRepeats = 10;
    static constexpr size_t kDefaultWarmupIterations = 10;

    struct Config {
        bool enabled = true;
        platform::MeasurementEnvironmentOptions environment;
        size_t min_offset = kDefaultMinOffest;
        size_t max_offset = kDefaultMaxOffest;
        size_t buffer_size = kDefaultBufferSize;
        size_t offset_step = kDefaultOffsetStep;
        size_t iterations = kDefaultIterations;
        size_t repeats = kDefaultRepeats;
        size_t warmup_iterations = kDefaultWarmupIterations;
        int alignment = kDefaultAlignment;
        double misprediction_saturation_threshold = 0.01;
        double misprediction_growth_threshold = 0.005;
        double time_growth_ratio = 1.20;
        size_t time_stability_points = 3;
        size_t coarse_ignore_first = 2;
    };

    StoreToLoadForwardingMeasurer() : StoreToLoadForwardingMeasurer(Config{}) {}
    explicit StoreToLoadForwardingMeasurer(Config config) : config_(std::move(config)) {
        SPDLOG_INFO("[{}] configured: min_offset = {}, max_offset = {}, offset_step = {}, iterations={}, "
                    "repeats={}, alignment={}",
                    name(), config_.min_offset, config_.max_offset, config_.offset_step, config_.iterations, config_.repeats,
                    config_.alignment);
    }

    std::string_view name() const noexcept override { return "branch target buffer"; }

    void measure(core::CpuInfoData& data) override {
        SPDLOG_INFO("[{}] starting BTB size measurement", name());

        platform::ScopedMeasurementEnvironment environment{config_.environment};

        auto s2l_fwd_events = platform::discover_s2l_forwarding_events();
        bool has_s2l_fwd = false;
        if (s2l_fwd_events.empty()) {
            SPDLOG_WARN("[{}] No s2l_fwd events found. Falling back to time-only measurement.", name());
        } else {
            SPDLOG_INFO("[{}] Found {} s2l_fwd events", name(), s2l_fwd_events.size());
            has_s2l_fwd = true;
        }

        std::unique_ptr<platform::pmc::PmcGroup> pmc;
        if (has_s2l_fwd) {
            pmc = platform::pmc::PmcGroup::create_raw(s2l_fwd_events);
            if (!pmc) {
                SPDLOG_WARN("[{}] failed to open s2l_fwd counters, falling back to time-only", name());
                has_s2l_fwd = false;
            }
        }

        std::vector<StoreToLoadForwardingResult> coarse_results;
        std::vector<size_t> coarse_counts;
        size_t store_forward_idx = std::string::npos;
        if (has_s2l_fwd) {
            for (size_t i = 0; i < s2l_fwd_events.size(); ++i) {
                if (s2l_fwd_events[i].find("store_forward") != std::string::npos) store_forward_idx = i;
            }
        }

        for (size_t offset = config_.min_offset; offset < config_.max_offset; offset += config_.offset_step) {
            StoreToLoadForwardingResult res = run_test(offset, pmc.get(), s2l_fwd_events);
            coarse_counts.push_back(offset);
            coarse_results.push_back(res);
        }

        size_t result = detectS2LFWDSaturation(coarse_results, coarse_counts, s2l_fwd_events, store_forward_idx);

        data.s2l_fwd_size = result;
        SPDLOG_INFO("[{}] estimated store-to-load forwarding size: {} s2l_fwd", name(), result);
        SPDLOG_INFO("[{}] measurement complete", name());
    }

  private:
    Config config_;

    StoreToLoadForwardingResult run_test(size_t offset, platform::pmc::PmcGroup* pmc, const std::vector<std::string>& s2l_fwd_events) {
        if (offset >= kDefaultBufferSize)
            throw std::runtime_error("offset is out of buffer range");

        std::vector<double> ticks_per_mem_access;
        std::vector<uint64_t> raw_ticks;
        std::vector<std::vector<uint64_t>> all_counts;

        alignas(64) size_t buffer[kDefaultBufferSize];
        volatile size_t dummy = 0;

        for (size_t i = 0; i < config_.warmup_iterations; ++i) {
            *buffer = i;
            dummy = *(buffer+offset);
        }

        for (size_t r = 0; r < config_.repeats; ++r) {
            if (pmc) {
                pmc->reset();
                pmc->enable();
            }

            uint64_t start_ticks = platform::arch::tick();
            for (size_t iteration = 0; iteration <= config_.iterations; ++iteration) {
                *buffer = iteration;
                dummy = *(buffer+offset);
            }
            uint64_t end_ticks = platform::arch::tick();

            if (pmc) pmc->disable();

            uint64_t ticks = end_ticks - start_ticks;
            raw_ticks.push_back(ticks);
            ticks_per_mem_access.push_back(static_cast<double>(ticks) / config_.iterations);

            if (pmc) {
                auto cv = pmc->read();
                all_counts.push_back(std::move(cv.values));
            }
        }

        (void)dummy;

        double avg_ticks_per_mem_access = std::accumulate(ticks_per_mem_access.begin(), ticks_per_mem_access.end(), 0.0) / config_.repeats;
        double avg_raw_ticks = std::accumulate(raw_ticks.begin(), raw_ticks.end(), 0.0) / config_.repeats;

        double ticks_std = 0.0;
        for (double t : ticks_per_mem_access) {
            double diff = t - avg_ticks_per_mem_access;
            ticks_std += diff * diff;
        }
        ticks_std = std::sqrt(ticks_std / config_.repeats);

        std::vector<uint64_t> avg_events_counts;
        if (!all_counts.empty()) {
            avg_events_counts.resize(all_counts[0].size(), 0);
            for (const auto& counts : all_counts) {
                for (size_t i = 0; i < counts.size(); ++i) {
                    avg_events_counts[i] += counts[i];
                }
            }
            for (size_t i = 0; i < avg_events_counts.size(); ++i) {
                avg_events_counts[i] /= config_.repeats;
            }
        }

        SPDLOG_INFO("[{}] offset={}: avg_ticks_per_mem_access={:.4g} (std={:.4g})", name(), offset, avg_ticks_per_mem_access, ticks_std);
        if (!avg_events_counts.empty()) {
            for (size_t i = 0; i < s2l_fwd_events.size(); ++i) {
                SPDLOG_INFO("  {} avg = {:.4g}", s2l_fwd_events[i], static_cast<double>(avg_events_counts[i]));
            }
        }

        return {avg_ticks_per_mem_access, avg_raw_ticks, ticks_std, std::move(avg_events_counts)};
    }

    size_t detectS2LFWDSaturation(
        const std::vector<StoreToLoadForwardingResult>& results,
        const std::vector<size_t>& offsets,
        const std::vector<std::string>& event_names,
        size_t store_forward_idx) const {

        if (results.empty() || offsets.empty()) {
            SPDLOG_WARN("[{}] No results to analyze, returning max offset", name());
            return config_.max_offset;
        }

        const size_t iterations = config_.iterations;
        const double time_growth_ratio = config_.time_growth_ratio;
        const size_t stability_points = config_.time_stability_points;
        const double saturation_threshold = config_.misprediction_saturation_threshold; // re-used for counter ratio

        // ----- 1. Time‑based analysis -----
        // Find baseline latency – average of first few offsets (assumed to be in forwarding regime)
        const size_t baseline_window = std::min<size_t>(3, results.size());
        double baseline_latency = 0.0;
        for (size_t i = 0; i < baseline_window; ++i) {
            baseline_latency += results[i].avg_ticks_per_block;
        }
        baseline_latency /= baseline_window;

        size_t time_saturation = offsets.back();
        size_t consecutive_high = 0;
        for (size_t i = 0; i < results.size(); ++i) {
            double cur_lat = results[i].avg_ticks_per_block;
            if (cur_lat > baseline_latency * time_growth_ratio) {
                ++consecutive_high;
                if (consecutive_high >= stability_points) {
                    time_saturation = offsets[i - stability_points + 1];
                    break;
                }
            } else {
                consecutive_high = 0;
            }
        }

        // ----- 2. Counter‑based analysis (if we have a store_forward event) -----
        size_t counter_saturation = offsets.back();
        if (store_forward_idx != std::string::npos && store_forward_idx < event_names.size()) {
            for (size_t i = 0; i < results.size(); ++i) {
                if (i >= results[i].avg_events_counts.size()) continue;
                uint64_t forward_blocked = results[i].avg_events_counts[store_forward_idx];
                double blocked_ratio = static_cast<double>(forward_blocked) / iterations;
                if (blocked_ratio > saturation_threshold) {
                    counter_saturation = offsets[i];
                    break;
                }
            }
        }

        // ----- 3. Combine results -----
        size_t final_saturation = offsets.back();
        if (store_forward_idx != std::string::npos) {
            // Prefer counter result if it's earlier (more precise), otherwise take the time result
            final_saturation = std::min(time_saturation, counter_saturation);
            SPDLOG_INFO("[{}] Time‑based saturation = {}, counter‑based saturation = {}, final = {}",
                        name(), time_saturation, counter_saturation, final_saturation);
        } else {
            final_saturation = time_saturation;
            SPDLOG_INFO("[{}] No store_forward counter, using time‑based saturation = {}", name(), final_saturation);
        }

        return final_saturation;
    }
};

} // namespace silicon_probe::branch_target_buffer
