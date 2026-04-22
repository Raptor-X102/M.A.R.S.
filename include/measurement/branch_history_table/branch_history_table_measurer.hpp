#pragma once

#include "infra/logging.hpp"
#include "measurement/core/measurer.hpp"
#include "platform/arch.hpp"
#include "platform/cpu_vendor.hpp"
#include "platform/os.hpp"
#include "platform/pmc.hpp"

#include <vector>
#include <random>
#include <cmath>
#include <algorithm>

namespace silicon_probe::branch_history_table {

class BranchHistoryTableMeasurer final : public core::Measurer {
  public:
    static constexpr size_t kDefaultMinPeriod = 16;
    static constexpr size_t kDefaultMaxPeriod = 32 * 1024;
    static constexpr size_t kDefaultPeriodCoef = 2; // geometric progression
    static constexpr size_t kDefaultIterations = 100'000'000;
    static constexpr unsigned int kPatternSeed = 123;
    static constexpr double kThreshold035 = 0.35;
    static constexpr double kThreshold040 = 0.40;
    static constexpr double kBaselineFraction = 1.0 / 3.0;
    static constexpr size_t kBaselineMaxSamples = 4;
    static constexpr double kBaselineOffset = 0.15;
    static constexpr double kMinDelta = 0.05;

    struct Config {
        bool enabled = true;
        platform::MeasurementEnvironmentOptions environment;
        size_t min_period = kDefaultMinPeriod;
        size_t max_period = kDefaultMaxPeriod;
        size_t period_coeff = kDefaultPeriodCoef;
        size_t iterations = kDefaultIterations;
    };

  private:
    Config config_;

    struct RawResult {
        size_t period;
        double miss_per_iter; // branch misses per iteration (0..0.5)
    };

  public:
    BranchHistoryTableMeasurer() : BranchHistoryTableMeasurer(Config{}) {}
    explicit BranchHistoryTableMeasurer(Config config) : config_(std::move(config)) {
        SPDLOG_INFO("[{}] configured: min_period={}, max_period={}, coeff={}, iterations={}", name(), config_.min_period,
                    config_.max_period, config_.period_coeff, config_.iterations);
    }

    std::string_view name() const noexcept override { return "branch history table"; }

    void measure(core::CpuInfoData& data) override {
        SPDLOG_INFO("[{}] starting Branch History Table size measurement", name());

        platform::ScopedMeasurementEnvironment environment{config_.environment};

        if (!platform::pmc::PmcGroup::is_supported()) {
            SPDLOG_ERROR("perf_event_open not supported");
            return;
        }

        auto pmc = platform::pmc::PmcGroup::create({platform::pmc::EventType::BRANCH_MISSES});
        if (!pmc) {
            SPDLOG_ERROR("Failed to open branch-misses counter");
            return;
        }

        // Prepare random pattern generator
        std::mt19937 rng(kPatternSeed);
        std::uniform_int_distribution<int> dist(0, 1);
        std::vector<bool> pattern(config_.max_period); // reusable buffer
        std::vector<RawResult> results;

        for (size_t period = config_.min_period; period <= config_.max_period; period *= config_.period_coeff) {
            // Fill pattern for this period
            for (size_t i = 0; i < period; ++i) {
                pattern[i] = (dist(rng) == 1);
            }

            volatile int sink = 0;

            pmc->reset();
            pmc->enable();

            for (size_t i = 0; i < config_.iterations; ++i) {
                if (pattern[i % period]) {
                    sink += 1;
                } else {
                    sink += 0;
                }
            }

            pmc->disable();
            auto values = pmc->read();

            if (!values.valid || values.values.empty()) {
                SPDLOG_WARN("[{}] failed to read counter for period={}", name(), period);
                continue;
            }

            uint64_t misses = values.values[0];
            double miss_per_iter = static_cast<double>(misses) / config_.iterations;

            results.push_back({period, miss_per_iter});

            SPDLOG_INFO("[{}] period={:5d}  misses={:12}  miss_per_iter={:.4f}", name(), period, misses, miss_per_iter);
        }

        if (results.empty()) {
            SPDLOG_ERROR("[{}] no valid data collected", name());
            return;
        }

        int bht_size = detectBHTSaturation(results);
        if (bht_size < 0) {
            SPDLOG_ERROR("[{}] could not detect BHT saturation in period range [{}, {}]", name(), config_.min_period, config_.max_period);
        } else {
            SPDLOG_INFO("[{}] Branch History Table effective length ≈ {} entries", name(), bht_size);
            data.bht_size = bht_size;
        }

        SPDLOG_INFO("[{}] measurement complete", name());
    }

  private:
    // Multiple estimation methods for BHT saturation point
    struct BHTEstimates {
        int threshold_035 = -1;               // miss_rate > 0.35
        int threshold_040 = -1;               // miss_rate > 0.40
        int threshold_baseline_plus_015 = -1; // baseline + 0.15
        int max_delta = -1;                   // largest increase in miss_rate
        int inflection_point = -1;            // max second derivative (log scale)
        int median_cross = -1;                // miss_rate crosses 0.4
        int best_guess = -1;
    };

    int detectBHTSaturation(const std::vector<RawResult>& results) const {
        if (results.size() < 4) {
            SPDLOG_WARN("[{}] insufficient data points for BHT detection", name());
            return -1;
        }

        // Prepare data: periods and miss rates
        std::vector<double> periods, log2periods, misses;
        for (const auto& r : results) {
            periods.push_back(static_cast<double>(r.period));
            log2periods.push_back(std::log2(r.period));
            misses.push_back(r.miss_per_iter);
        }

        // Baseline: median of first few points (small periods, low misses)
        size_t baseline_cnt = static_cast<size_t>(std::ceil(static_cast<double>(results.size()) * kBaselineFraction));
        baseline_cnt = std::min(kBaselineMaxSamples, baseline_cnt);
        if (baseline_cnt < 2) baseline_cnt = results.size() / 2;
        std::vector<double> baseline_vals(misses.begin(), misses.begin() + baseline_cnt);
        std::sort(baseline_vals.begin(), baseline_vals.end());
        double baseline = baseline_vals[baseline_vals.size() / 2];

        BHTEstimates est;

        // Method 1: Absolute threshold 0.35
        for (size_t i = 0; i < misses.size(); ++i) {
            if (misses[i] > kThreshold035) {
                est.threshold_035 = static_cast<int>(periods[i]);
                break;
            }
        }

        // Method 2: Absolute threshold 0.40
        for (size_t i = 0; i < misses.size(); ++i) {
            if (misses[i] > kThreshold040) {
                est.threshold_040 = static_cast<int>(periods[i]);
                break;
            }
        }

        // Method 3: Baseline + 0.15
        double thresh_baseline = baseline + kBaselineOffset;
        for (size_t i = 0; i < misses.size(); ++i) {
            if (misses[i] > thresh_baseline) {
                est.threshold_baseline_plus_015 = static_cast<int>(periods[i]);
                break;
            }
        }

        // Method 4: Maximum delta (largest increase between consecutive points)
        double max_delta = 0.0;
        size_t max_delta_idx = 0;
        for (size_t i = 1; i < misses.size(); ++i) {
            double delta = misses[i] - misses[i - 1];
            if (delta > max_delta) {
                max_delta = delta;
                max_delta_idx = i;
            }
        }
        if (max_delta_idx > 0 && max_delta > kMinDelta) {
            est.max_delta = static_cast<int>(periods[max_delta_idx]);
        }

        // Method 5: Inflection point – maximum second derivative on log2 scale
        std::vector<double> second_deriv;
        for (size_t i = 1; i < misses.size() - 1; ++i) {
            double dx = log2periods[i + 1] - log2periods[i - 1];
            if (dx > 0) {
                double dy = misses[i + 1] - misses[i - 1];
                double deriv = dy / dx;
                // Second derivative approximated by difference of first derivatives
                if (i > 1) {
                    double prev_deriv = (misses[i] - misses[i - 2]) / (log2periods[i] - log2periods[i - 2]);
                    second_deriv.push_back(deriv - prev_deriv);
                } else {
                    second_deriv.push_back(0);
                }
            }
        }
        if (!second_deriv.empty()) {
            auto max_it = std::max_element(second_deriv.begin(), second_deriv.end());
            size_t idx = std::distance(second_deriv.begin(), max_it) + 1; // +1 because we started at i=1
            if (idx < periods.size()) {
                est.inflection_point = static_cast<int>(periods[idx]);
            }
        }

        // Method 6: Median crossing – find where miss_rate crosses median of high range
        // Actually simpler: find where it crosses 0.4 (already done), but also find closest to 0.5
        for (size_t i = 1; i < misses.size(); ++i) {
            if (misses[i - 1] < kThreshold040 && misses[i] >= kThreshold040) {
                // Linear interpolation to find exact period at 0.4
                double t = (kThreshold040 - misses[i - 1]) / (misses[i] - misses[i - 1]);
                double interp_period = periods[i - 1] + t * (periods[i] - periods[i - 1]);
                est.median_cross = static_cast<int>(std::round(interp_period));
                break;
            }
        }

        // Log all estimates
        SPDLOG_INFO("[{}] BHT estimation methods:", name());
        SPDLOG_INFO("  baseline miss rate = {:.4f}", baseline);
        SPDLOG_INFO("  threshold 0.35        → {} entries", est.threshold_035);
        SPDLOG_INFO("  threshold 0.40        → {} entries", est.threshold_040);
        SPDLOG_INFO("  baseline+{:.2f}       → {} entries", kBaselineOffset, est.threshold_baseline_plus_015);
        SPDLOG_INFO("  max delta (increase)  → {} entries (delta={:.4f})", est.max_delta, max_delta);
        SPDLOG_INFO("  inflection point (2nd deriv) → {} entries", est.inflection_point);
        SPDLOG_INFO("  median cross at {:.2f} → {} entries", kThreshold040, est.median_cross);

        // Choose best guess: usually threshold 0.40 is reliable
        int best = est.threshold_040;
        if (best < 0) best = est.threshold_035;
        if (best < 0) best = est.median_cross;
        if (best < 0) best = est.inflection_point;
        if (best < 0) best = est.max_delta;

        SPDLOG_INFO("[{}] best estimate (0.40 threshold) = {} entries", name(), best);
        return best;
    }
};

} // namespace silicon_probe::branch_history_table
