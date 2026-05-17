#include "measurement/branch_history_table/branch_history_table_measurer.hpp"

#include "infra/logging.hpp"
#include "platform/arch.hpp"
#include "platform/pmc.hpp"

namespace silicon_probe::branch_history_table {

BranchHistoryTableMeasurer::BranchHistoryTableMeasurer()
    : BranchHistoryTableMeasurer(Config{}) {}

BranchHistoryTableMeasurer::BranchHistoryTableMeasurer(Config config)
    : config_(std::move(config)) {
    validateConfig();
    SPDLOG_INFO("[{}] configured: min_period={}, max_period={}, coeff={}, iterations={}", name(),
                config_.min_period, config_.max_period, config_.period_coeff, config_.iterations);
}

std::string_view BranchHistoryTableMeasurer::name() const noexcept {
    return "branch history table";
}

void BranchHistoryTableMeasurer::validateConfig() {
    if (config_.min_period == 0) {
        SPDLOG_WARN("[{}] min_period=0, set to {}", name(), kDefaultMinPeriod);
        config_.min_period = kDefaultMinPeriod;
    }
    if (config_.max_period < config_.min_period) {
        SPDLOG_WARN("[{}] max_period < min_period, set to {}", name(), kDefaultMaxPeriod);
        config_.max_period = kDefaultMaxPeriod;
    }
    if (config_.period_coeff < 1.1) {
        SPDLOG_WARN("[{}] period_coeff too small (<1.1), set to {}", name(), kDefaultPeriodCoef);
        config_.period_coeff = kDefaultPeriodCoef;
    }
    if (config_.iterations == 0) {
        SPDLOG_WARN("[{}] iterations=0, set to {}", name(), kDefaultIterations);
        config_.iterations = kDefaultIterations;
    }
    if (config_.abs_threshold <= 0.0 || config_.abs_threshold >= 1.0) {
        SPDLOG_WARN("[{}] abs_threshold={} invalid, set to 0.2", name(), config_.abs_threshold);
        config_.abs_threshold = 0.2;
    }
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

    std::vector<BranchHistoryTableResult> results;

    for (size_t period = config_.min_period; period <= config_.max_period; 
            period = static_cast<size_t>(static_cast<double>(period) * config_.period_coeff)) {
        std::vector<bool> pattern(period);
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

    auto bht_size_opt = detectBHTSaturation(results);
    if (!bht_size_opt.has_value()) {
        SPDLOG_ERROR("[{}] could not detect BHT saturation ...", name());
    } else {
        int bht_size = *bht_size_opt;
        SPDLOG_INFO("[{}] Branch History Table effective length ≈ {} entries", name(), bht_size);
        data.bht_size = bht_size;
    }

    SPDLOG_INFO("[{}] measurement complete", name());
}

std::optional<int> BranchHistoryTableMeasurer::detectBHTSaturation(
    const std::vector<BranchHistoryTableResult>& results) const
{
    if (results.size() < 4) {
        SPDLOG_WARN("[{}] insufficient data points", name());
        return std::nullopt;
    }

    std::vector<double> periods, misses;
    for (const auto& r : results) {
        periods.push_back(static_cast<double>(r.period));
        misses.push_back(r.miss_per_iter);
    }

    // Primary method: find doubling step with maximum delta (steepest rise)
    std::optional<int> best;
    double max_delta = -1.0;

    for (size_t i = 1; i < results.size(); ++i) {
        double ratio = periods[i] / periods[i-1];
        if (ratio >= 1.9 && ratio <= 2.1) {      // period doubled (or almost)
            double delta = misses[i] - misses[i-1];
            if (delta > max_delta) {
                max_delta = delta;
                best = static_cast<int>(periods[i-1]);  // period before the jump
            }
        }
    }

    if (best.has_value()) {
        SPDLOG_INFO("[{}] BHT detection: max delta method → period {} (delta={:.4f})",
                    name(), *best, max_delta);
    } else {
        // Fallback: first period where miss rate exceeds absolute threshold
        const double abs_thresh = config_.abs_threshold > 0.0 ? config_.abs_threshold : 0.2;
        for (size_t i = 0; i < results.size(); ++i) {
            if (misses[i] >= abs_thresh) {
                best = static_cast<int>(periods[i]);
                SPDLOG_INFO("[{}] BHT detection: absolute threshold ({}) → period {}",
                            name(), abs_thresh, *best);
                break;
            }
        }
    }

    // Last resort: use the largest period
    if (!best.has_value()) {
        best = static_cast<int>(periods.back());
        SPDLOG_WARN("[{}] BHT detection: no reliable method, using largest period {}",
                    name(), *best);
    }

    // Optional: round up to nearest power of two (typical BHT sizes)
    int rounded = 1;
    while (rounded < *best) rounded <<= 1;
    if (rounded != *best) {
        SPDLOG_INFO("[{}] rounding BHT size from {} to {} (power of two)", name(), *best, rounded);
        *best = rounded;
    }

    SPDLOG_INFO("[{}] final BHT estimate = {} entries", name(), *best);
    return best;
}

} // namespace silicon_probe::branch_history_table
