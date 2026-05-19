#include "measurement/uops_cache/uops_cache_measurer.hpp"

#include <numeric>

namespace silicon_probe::uops_cache {

UopsCacheMeasurer::UopsCacheMeasurer() : UopsCacheMeasurer(Config{}) {}

UopsCacheMeasurer::UopsCacheMeasurer(Config config) : config_(std::move(config)) {
    validateConfig();
    SPDLOG_DEBUG(
        "[{}] configured: min_instr_cnt = {}, max_instr_cnt = {}, instr_step = {}, iterations={}, "
        "repeats={}, instr = [{}, {}]",
        name(), config_.min_instr_cnt, config_.max_instr_cnt, config_.instr_step, config_.iterations, config_.repeats,
        static_cast<int>(config_.instr.instr_type), config_.instr.instr_name
    );
}

std::string_view UopsCacheMeasurer::name() const noexcept { return "uops cache"; }

void UopsCacheMeasurer::validateConfig() {
    if (config_.min_instr_cnt == 0)
        config_.min_instr_cnt = kDefaultMinInstrCnt;
    if (config_.max_instr_cnt <= config_.min_instr_cnt)
        config_.max_instr_cnt = kDefaultMaxInstrCnt;
    if (config_.instr_step == 0)
        config_.instr_step = kDefaultInstrStep;
    if (config_.iterations == 0)
        config_.iterations = kDefaultIterations;
    if (config_.repeats == 0)
        config_.repeats = kDefaultRepeats;
    if (config_.dsb_share_stop < 0.0 || config_.dsb_share_stop > 1.0)
        config_.dsb_share_stop = 0.3;
    if (config_.dsb_share_refine < 0.0 || config_.dsb_share_refine > 1.0)
        config_.dsb_share_refine = 0.8;
    if (config_.dsb_drop_significant < 0.0 || config_.dsb_drop_significant > 1.0)
        config_.dsb_drop_significant = 0.2;
    if (config_.coarse_ignore_first == 0)
        config_.coarse_ignore_first = 1;
}

uint8_t UopsCacheMeasurer::get_instr_uops_size(InstrType type) {
    switch (type) {
        case InstrType::NOP:
            return 1;
        // implement if you want to use smthing else
        default:
            return 1;
    }
}

void UopsCacheMeasurer::measure(shared_types::CpuInfoData& data) {
    if (!config_.enabled) {
        SPDLOG_INFO("[{}] measurement disabled by config", name());
        return;
    }

    SPDLOG_INFO("[{}] starting uops cache size measurement", name());

    platform::ScopedMeasurementEnvironment environment{config_.environment};

    auto uops_events = platform::discover_uops_events(data);
    if (uops_events.size() != 2) {
        SPDLOG_ERROR("[{}] expected 2 uops events (MITE and DSB), got {}", name(), uops_events.size());
        data.uops_cache_size = 0;
        return;
    }

    // discover_uops_events returns [MITE_UOPS, DSB_UOPS]
    SPDLOG_DEBUG("[{}] MITE event: {}, DSB event: {}", name(), uops_events[0], uops_events[1]);

    auto pmc = platform::pmc::PmcGroup::create_raw(uops_events);
    if (!pmc) {
        SPDLOG_ERROR("[{}] failed to open uops counters", name());
        data.uops_cache_size = 0;
        return;
    }

    // Coarse scan with early stop
    std::vector<UopsCacheResult> coarse_results;
    std::vector<size_t> coarse_counts;
    bool saturation_reached = false;

    for (size_t instr_cnt = config_.min_instr_cnt; instr_cnt < config_.max_instr_cnt; instr_cnt += config_.instr_step) {
        UopsCacheResult res = run_test(instr_cnt, pmc.get(), uops_events);
        coarse_counts.push_back(instr_cnt);
        coarse_results.push_back(res);

        uint64_t mite = res.avg_events_counts[0];
        uint64_t dsb  = res.avg_events_counts[1];
        double share  = (dsb + mite) > 0 ? static_cast<double>(dsb) / (dsb + mite) : 0.0;

        if (share < config_.dsb_share_stop && coarse_counts.size() >= config_.coarse_ignore_first) {
            saturation_reached = true;
            SPDLOG_DEBUG(
                "[{}] DSB share dropped to {:.3f} at instr_cnt={}, stopping coarse scan", name(), share, instr_cnt
            );
            break;
        }
    }

    if (!saturation_reached && coarse_counts.size() >= 3) {
        SPDLOG_WARN("[{}] saturation not reached within range, trying to find maximum drop", name());
    }

    size_t approx_saturation = findApproxSaturation(coarse_counts, coarse_results);
    if (approx_saturation == 0) {
        platform::arch::release_uops_cache_code();
        data.uops_cache_size = 0;
        SPDLOG_WARN("[{}] could not detect saturation point", name());
        return;
    }

    size_t refined = refineSaturation(approx_saturation, pmc.get(), uops_events);
    platform::arch::release_uops_cache_code();

    data.uops_cache_size = refined * get_instr_uops_size(config_.instr.instr_type);
    SPDLOG_INFO("[{}] estimated uop cache size: {} uops", name(), *data.uops_cache_size);
    SPDLOG_INFO("[{}] measurement complete", name());
}

UopsCacheResult UopsCacheMeasurer::run_test(
    size_t instr_cnt, platform::pmc::PmcGroup* pmc, const std::vector<std::string>& uops_events
) {
    void* func = platform::arch::generate_uops_cache_code(instr_cnt, config_.iterations, {config_.instr.instr_type});
    if (!func) {
        SPDLOG_ERROR("[{}] failed to generate test function for instr_cnt={}", name(), instr_cnt);
        return {};
    }

    auto f = reinterpret_cast<void (*)()>(func);
    for (size_t i = 0; i < config_.warmup_iterations; ++i)
        f();

    std::vector<double> ticks_per_instr;
    std::vector<std::vector<uint64_t>> all_counts;

    for (size_t r = 0; r < config_.repeats; ++r) {
        if (pmc) {
            pmc->reset();
            pmc->enable();
        }

        uint64_t start_ticks = platform::arch::tick();
        f();
        uint64_t end_ticks = platform::arch::tick();

        if (pmc)
            pmc->disable();

        uint64_t ticks       = end_ticks - start_ticks;
        uint64_t total_instr = instr_cnt * config_.iterations;
        ticks_per_instr.push_back(static_cast<double>(ticks) / total_instr);

        if (pmc) {
            auto cv = pmc->read();
            all_counts.push_back(std::move(cv.values));
        }
    }

    double avg_ticks_per_instr = std::accumulate(ticks_per_instr.begin(), ticks_per_instr.end(), 0.0) / config_.repeats;

    double ticks_std = 0.0;
    for (double t : ticks_per_instr) {
        double diff = t - avg_ticks_per_instr;
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

    SPDLOG_DEBUG(
        "[{}] instr_cnt={}: avg_ticks_per_instr={:.4g} (std={:.4g})", name(), instr_cnt, avg_ticks_per_instr, ticks_std
    );
    if (!avg_events_counts.empty()) {
        for (size_t i = 0; i < uops_events.size(); ++i) {
            SPDLOG_DEBUG("  {} avg = {:.4g}", uops_events[i], static_cast<double>(avg_events_counts[i]));
        }
        if (uops_events.size() >= 2) {
            // discover_uops_events returns [MITE_UOPS, DSB_UOPS]
            double mite = static_cast<double>(avg_events_counts[0]);
            double dsb  = static_cast<double>(avg_events_counts[1]);
            SPDLOG_DEBUG("DSB share = {:.4g}", dsb / (dsb + mite));
        }
    }

    return {avg_ticks_per_instr, ticks_std, std::move(avg_events_counts)};
}

size_t UopsCacheMeasurer::findApproxSaturation(
    const std::vector<size_t>& counts, const std::vector<UopsCacheResult>& results
) {
    if (counts.size() < 3 || results.empty())
        return 0;

    if (results[0].avg_events_counts.size() < 2)
        return 0;

    std::vector<double> dsb_share;
    for (const auto& r : results) {
        uint64_t mite = r.avg_events_counts[0];
        uint64_t dsb  = r.avg_events_counts[1];
        double share  = (dsb + mite) > 0 ? static_cast<double>(dsb) / (dsb + mite) : 0.0;
        dsb_share.push_back(share);
    }

    double max_drop = 0.0;
    size_t drop_idx = 0;
    for (size_t i = config_.coarse_ignore_first; i < dsb_share.size(); ++i) {
        double drop = dsb_share[i - 1] - dsb_share[i];
        if (drop > max_drop) {
            max_drop = drop;
            drop_idx = i;
        }
    }

    if (max_drop > config_.dsb_drop_significant) {
        return counts[drop_idx - 1];
    }
    return 0;
}

size_t UopsCacheMeasurer::refineSaturation(
    size_t approx, platform::pmc::PmcGroup* pmc, const std::vector<std::string>& uops_events
) {
    if (!pmc)
        return approx;

    size_t left  = (approx > config_.instr_step) ? approx - config_.instr_step : config_.min_instr_cnt;
    size_t right = approx + config_.instr_step;
    if (right > config_.max_instr_cnt)
        right = config_.max_instr_cnt;

    size_t answer = left;
    while (left <= right) {
        size_t mid          = left + (right - left) / 2;
        UopsCacheResult res = run_test(mid, pmc, uops_events);
        if (res.avg_events_counts.empty())
            break;

        uint64_t mite = res.avg_events_counts[0];
        uint64_t dsb  = res.avg_events_counts[1];
        double share  = (dsb + mite) > 0 ? static_cast<double>(dsb) / (dsb + mite) : 0.0;

        if (share >= config_.dsb_share_refine) {
            answer = mid;
            left   = mid + 1;
        } else {
            right = mid - 1;
        }
    }
    return answer;
}

}  // namespace silicon_probe::uops_cache
