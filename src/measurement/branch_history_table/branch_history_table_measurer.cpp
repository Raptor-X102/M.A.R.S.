#include "measurement/branch_history_table/branch_history_table_measurer.hpp"

#include "infra/logging.hpp"
#include "platform/arch.hpp"
#include "platform/pmc.hpp"

namespace silicon_probe::branch_history_table {

BranchHistoryTableMeasurer::BranchHistoryTableMeasurer() : BranchHistoryTableMeasurer(Config{}) {}

BranchHistoryTableMeasurer::BranchHistoryTableMeasurer(Config config) : config_(std::move(config)) {
    SPDLOG_INFO(
        "[{}] configured: min_period={}, max_period={}, coeff={}, iterations={}", name(), config_.min_period,
        config_.max_period, config_.period_coeff, config_.iterations
    );
}

std::string_view BranchHistoryTableMeasurer::name() const noexcept { return "branch history table"; }

void BranchHistoryTableMeasurer::measure(shared_types::CpuInfoData& data) {
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
    std::vector<bool> pattern(config_.max_period);
    std::vector<BranchHistoryTableResult> results;

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

        uint64_t misses      = values.values[0];
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
        SPDLOG_ERROR(
            "[{}] could not detect BHT saturation in period range [{}, {}]", name(), config_.min_period,
            config_.max_period
        );
    } else {
        SPDLOG_INFO("[{}] Branch History Table effective length ≈ {} entries", name(), bht_size);
        data.bht_size = bht_size;
    }

    SPDLOG_INFO("[{}] measurement complete", name());
}

int BranchHistoryTableMeasurer::detectBHTSaturation(const std::vector<BranchHistoryTableResult>& results) const {
    if (results.size() < 4) {
        SPDLOG_WARN("[{}] insufficient data points for BHT detection", name());
        return -1;
    }

    std::vector<double> periods, misses;
    for (const auto& r : results) {
        periods.push_back(static_cast<double>(r.period));
        misses.push_back(r.miss_per_iter);
    }

    // 1. Baseline: use the smallest observed miss rate (typically first small periods)
    double baseline = *std::min_element(misses.begin(), misses.end());

    // 2. Asymptotic miss rate: average of last 3 points (or last point if fewer)
    size_t n        = misses.size();
    double max_miss = 0.0;
    if (n >= 3) {
        double sum = 0.0;
        for (size_t i = n - 3; i < n; ++i)
            sum += misses[i];
        max_miss = sum / 3.0;
    } else {
        max_miss = misses.back();
    }

    // 3. Find period where miss rate reaches 90% of the rise from baseline to max_miss
    // Use 95% of the rise to saturation (more accurate for modern CPUs)
    const double saturation_fraction = 0.95;
    double threshold                 = baseline + saturation_fraction * (max_miss - baseline);
    int saturation_period            = -1;
    for (size_t i = 0; i < n; ++i) {
        if (misses[i] >= threshold) {
            saturation_period = static_cast<int>(periods[i]);
            break;
        }
    }

    // 4. Alternative method: first period where the derivative (increase per doubling) becomes small
    //    after a significant initial rise. This helps when 90% threshold is too early.
    int derivative_period            = -1;
    const double high_rise_threshold = 0.15;  // require at least 0.15 increase per doubling initially
    const double low_rise_threshold  = 0.03;  // after saturation, increase per doubling < 0.03
    bool seen_high_rise              = false;
    for (size_t i = 1; i < n; ++i) {
        double delta = misses[i] - misses[i - 1];
        // If period doubled (or roughly doubled, allow ratio 2x)
        double period_ratio = periods[i] / periods[i - 1];
        if (period_ratio >= 1.9 && period_ratio <= 2.1) {
            if (!seen_high_rise && delta >= high_rise_threshold) {
                seen_high_rise = true;
            }
            if (seen_high_rise && delta < low_rise_threshold) {
                derivative_period = static_cast<int>(periods[i - 1]);  // take the period before the drop
                break;
            }
        }
    }

    // 5. Sharp jump fallback (relaxed conditions)
    int sharp_jump = -1;
    for (size_t i = 1; i < n; ++i) {
        double period_ratio = periods[i] / periods[i - 1];
        if (misses[i] > 0.25 && misses[i - 1] < 0.10 && period_ratio >= 1.9 && period_ratio <= 2.1) {
            sharp_jump = static_cast<int>(periods[i - 1]);
            break;
        }
    }

    // Combine candidates: we prefer the saturation_period (90% threshold) but if derivative method
    // gives a larger period, it may be more accurate. Use the larger of the two if they are close,
    // otherwise fallback.
    int best = saturation_period;
    if (derivative_period > 0) {
        // If derivative period is within factor 2 of saturation period, take the larger one.
        if (best > 0 && derivative_period > best * 1.5) {
            SPDLOG_WARN(
                "[{}] derivative period {} is much larger than saturation period {}, using derivative", name(),
                derivative_period, best
            );
            best = derivative_period;
        } else if (derivative_period > best) {
            best = derivative_period;
        }
    }

    // If both failed, use sharp_jump
    if (best <= 0 && sharp_jump > 0) {
        best = sharp_jump;
    }

    if (best <= 0) {
        SPDLOG_WARN("[{}] no reliable BHT size found, defaulting to largest period", name());
        best = static_cast<int>(periods.back());
    }

    SPDLOG_INFO(
        "[{}] BHT estimation: baseline={:.4f}, max_miss={:.4f}, threshold={:.4f}", name(), baseline, max_miss, threshold
    );
    SPDLOG_INFO("  saturation (90% rise) period = {}", saturation_period);
    SPDLOG_INFO("  derivative (rise then plateau) period = {}", derivative_period);
    SPDLOG_INFO("  sharp jump period = {}", sharp_jump);
    SPDLOG_INFO("  selected BHT size = {}", best);

    return best;
}

}  // namespace silicon_probe::branch_history_table
