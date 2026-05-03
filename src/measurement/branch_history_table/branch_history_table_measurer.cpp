#include "measurement/branch_history_table/branch_history_table_measurer.hpp"

#include "infra/logging.hpp"
#include "platform/arch.hpp"
#include "platform/pmc.hpp"

namespace silicon_probe::branch_history_table {

BranchHistoryTableMeasurer::BranchHistoryTableMeasurer()
    : BranchHistoryTableMeasurer(Config{}) {}

BranchHistoryTableMeasurer::BranchHistoryTableMeasurer(Config config)
    : config_(std::move(config)) {
    SPDLOG_INFO("[{}] configured: min_period={}, max_period={}, coeff={}, iterations={}", name(),
                config_.min_period, config_.max_period, config_.period_coeff, config_.iterations);
}

std::string_view BranchHistoryTableMeasurer::name() const noexcept {
    return "branch history table";
}

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
        SPDLOG_ERROR("[{}] could not detect BHT saturation in period range [{}, {}]", name(), config_.min_period,
                     config_.max_period);
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

    size_t baseline_cnt = std::min(kBaselineMaxSamples,
                                   static_cast<size_t>(std::ceil(results.size() * kBaselineFraction)));
    if (baseline_cnt < 2) baseline_cnt = results.size() / 2;
    std::vector<double> baseline_vals(misses.begin(), misses.begin() + baseline_cnt);
    std::sort(baseline_vals.begin(), baseline_vals.end());
    double baseline = baseline_vals[baseline_vals.size() / 2];
    double threshold = baseline + kBaselineOffset;

    BHTEstimates est;

    // Method 1: Baseline + offset, take previous period if sharp jump
    for (size_t i = 1; i < misses.size(); ++i) {
        if (misses[i] > threshold) {
            if (misses[i] - misses[i-1] > 0.1 && misses[i-1] < 0.05) {
                est.baseline_offset = static_cast<int>(periods[i-1]);
            } else {
                est.baseline_offset = static_cast<int>(periods[i]);
            }
            break;
        }
    }

    // Method 2: Maximum delta (largest increase)
    double max_delta = 0.0;
    size_t max_delta_idx = 0;
    for (size_t i = 1; i < misses.size(); ++i) {
        double delta = misses[i] - misses[i-1];
        if (delta > max_delta) {
            max_delta = delta;
            max_delta_idx = i;
        }
    }
    if (max_delta > kMinDelta) {
        if (max_delta > 0.2 && max_delta_idx > 0 && misses[max_delta_idx-1] < 0.05) {
            est.max_delta = static_cast<int>(periods[max_delta_idx-1]);
        } else {
            est.max_delta = static_cast<int>(periods[max_delta_idx]);
        }
    }

    // Method 3: Inflection point (maximum second derivative on log2 scale)
    std::vector<double> second_deriv;
    for (size_t i = 1; i < misses.size() - 1; ++i) {
        double dx = std::log2(periods[i+1]) - std::log2(periods[i-1]);
        if (dx > 0) {
            double dy = misses[i+1] - misses[i-1];
            double deriv = dy / dx;
            if (i > 1) {
                double prev_deriv = (misses[i] - misses[i-2]) / (std::log2(periods[i]) - std::log2(periods[i-2]));
                second_deriv.push_back(deriv - prev_deriv);
            } else {
                second_deriv.push_back(0);
            }
        }
    }
    if (!second_deriv.empty()) {
        auto max_it = std::max_element(second_deriv.begin(), second_deriv.end());
        size_t idx = std::distance(second_deriv.begin(), max_it) + 1;
        if (idx < periods.size()) {
            est.inflection_point = static_cast<int>(periods[idx]);
        }
    }

    // Find sharp jump directly (fallback for when all methods overestimate)
    int sharp_jump = -1;
    for (size_t i = 1; i < misses.size(); ++i) {
        if (misses[i] > 0.3 && misses[i-1] < 0.05 && periods[i] / periods[i-1] == 2) {
            sharp_jump = static_cast<int>(periods[i-1]);
            break;
        }
    }

    std::vector<int> candidates;
    if (est.baseline_offset > 0) candidates.push_back(est.baseline_offset);
    if (est.max_delta > 0) candidates.push_back(est.max_delta);
    if (est.inflection_point > 0) candidates.push_back(est.inflection_point);

    if (candidates.empty()) {
        SPDLOG_WARN("[{}] no candidate found", name());
        return sharp_jump > 0 ? sharp_jump : -1;
    }

    int best = *std::min_element(candidates.begin(), candidates.end());

    if (sharp_jump > 0 && best > sharp_jump) {
        SPDLOG_WARN("[{}] all methods overestimated (min={}, sharp_jump={}), using sharp_jump",
                    name(), best, sharp_jump);
        best = sharp_jump;
    }

    SPDLOG_INFO("[{}] BHT estimation: baseline={:.4f}, threshold={:.4f}", name(), baseline, threshold);
    SPDLOG_INFO("  baseline+offset candidate = {}", est.baseline_offset);
    SPDLOG_INFO("  max delta candidate        = {} (delta={:.4f})", est.max_delta, max_delta);
    SPDLOG_INFO("  inflection point candidate = {}", est.inflection_point);
    SPDLOG_INFO("  selected BHT size = {}", best);

    return best;
}

} // namespace silicon_probe::branch_history_table
