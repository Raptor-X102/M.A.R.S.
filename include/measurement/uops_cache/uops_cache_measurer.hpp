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

namespace silicon_probe::uops_cache {

struct IstructionData {
    platform::arch::InstrType instr_type;
    std::string instr_name;
};

struct UopsCacheResult {
    double avg_ticks_per_instr;
    double avg_ticks_per_iter;
    double ticks_std;
    std::vector<uint64_t> avg_events_counts;
};

struct UopsCacheSaturationPoint {
    size_t size_uops;
    double confidence;
    std::string reasoning;
};

class UopsCacheMeasurer final : public core::Measurer {
public:
    using InstrType = platform::arch::InstrType;

    static constexpr size_t kDefaultMinInstrCnt = 1200;
    static constexpr size_t kDefaultMaxInstrCnt = 5000;
    static constexpr size_t kDefaultInstrStep = 100;
    static constexpr size_t kDefaultIterations = 100'000;
    static constexpr size_t kDefaultRepeats = 10;
    static constexpr size_t kDefaultWarmupIterations = 100;

    struct Config {
        platform::MeasurementEnvironmentOptions environment;
        size_t min_instr_cnt = kDefaultMinInstrCnt;
        size_t max_instr_cnt = kDefaultMaxInstrCnt;
        size_t instr_step = kDefaultInstrStep;
        size_t iterations = kDefaultIterations;
        size_t repeats = kDefaultRepeats;
        IstructionData instr = { InstrType::ADD_REG, "add reg" };
    };

    UopsCacheMeasurer() : UopsCacheMeasurer(Config{}) {}
    explicit UopsCacheMeasurer(Config config) : config_(std::move(config)) {
        SPDLOG_INFO("[{}] configured: min_instr_cnt = {}, max_instr_cnt = {}, instr_step = {}, iterations={}, repeats={}, instr = [{}, {}]",
                    name(), config_.min_instr_cnt, config_.max_instr_cnt, 
                    config_.instr_step, config_.iterations, config_.repeats, 
                    static_cast<int>(config_.instr.instr_type), config_.instr.instr_name);
    }

    std::string_view name() const noexcept override { return "uops cache"; }

    void measure(core::CpuInfoData& data) override {
        SPDLOG_INFO("[{}] starting uops cache size measurement", name());

        platform::ScopedMeasurementEnvironment environment{config_.environment};

        auto uops_events = platform::discover_uops_events();
        bool has_uops = false;
        if (uops_events.empty()) {
            SPDLOG_WARN("[{}] No uops events found. Falling back to time-only measurement.", name());
        } else {
            SPDLOG_INFO("[{}] Found {} uops events", name(), uops_events.size());
            has_uops = true;
        }

        std::unique_ptr<platform::pmc::PmcGroup> pmc;
        if (has_uops) {
            pmc = platform::pmc::PmcGroup::create_raw(uops_events);
            if (!pmc) {
                SPDLOG_WARN("[{}] failed to open uops counters, falling back to time-only", name());
                has_uops = false;
            }
        }

        // Coarse scan with early stop
        std::vector<UopsCacheResult> coarse_results;
        std::vector<size_t> coarse_counts;
        bool saturation_reached = false;
        size_t dsb_idx = std::string::npos, mite_idx = std::string::npos;
        if (has_uops) {
            for (size_t i = 0; i < uops_events.size(); ++i) {
                if (uops_events[i].find("dsb_uops") != std::string::npos) dsb_idx = i;
                if (uops_events[i].find("mite_uops") != std::string::npos) mite_idx = i;
            }
        }

        for (size_t instr_cnt = config_.min_instr_cnt; instr_cnt < config_.max_instr_cnt; instr_cnt += config_.instr_step) {
            UopsCacheResult res = run_test(instr_cnt, pmc.get(), uops_events);
            coarse_counts.push_back(instr_cnt);
            coarse_results.push_back(res);

            if (has_uops && dsb_idx != std::string::npos && mite_idx != std::string::npos && !res.avg_events_counts.empty()) {
                uint64_t dsb = res.avg_events_counts[dsb_idx];
                uint64_t mite = res.avg_events_counts[mite_idx];
                double share = (dsb + mite) > 0 ? static_cast<double>(dsb) / (dsb + mite) : 0.0;
                if (share < kDSBShareStop && coarse_counts.size() >= kCoarseIgnoreFirst) {
                    saturation_reached = true;
                    SPDLOG_INFO("[{}] DSB share dropped to {:.3f} at instr_cnt={}, stopping coarse scan", name(), share, instr_cnt);
                    break;
                }
            }
        }

        if (!saturation_reached && coarse_counts.size() >= 3) {
            SPDLOG_WARN("[{}] saturation not reached within range, using last points", name());
        }

        size_t approx_saturation = findApproxSaturation(coarse_counts, coarse_results, uops_events);
        if (approx_saturation == 0) {
            platform::arch::release_uops_cache_code();
            data.uops_cache_size = 0;
            SPDLOG_WARN("[{}] could not detect saturation point", name());
            return;
        }

        size_t refined = refineSaturation(approx_saturation, pmc.get(), uops_events);
        platform::arch::release_uops_cache_code();

        data.uops_cache_size = refined;
        SPDLOG_INFO("[{}] estimated uop cache size: {} uops", name(), refined);
        SPDLOG_INFO("[{}] measurement complete", name());
    }

private:
    static constexpr double kDSBShareHigh = 0.9;       // share above this is considered "good"
    static constexpr double kDSBShareLow = 0.5;        // share below this indicates saturation
    static constexpr double kDSBShareStop = 0.3;       // stop coarse scan when share falls below this
    static constexpr double kDSBShareRefine = 0.8;     // threshold for binary search (last good)
    static constexpr double kDSBDropSignificant = 0.2; // minimum drop to detect saturation
    static constexpr size_t kCoarseIgnoreFirst = 3;    // ignore first N coarse points (warmup)

    UopsCacheResult run_test(size_t instr_cnt, platform::pmc::PmcGroup* pmc,
                             const std::vector<std::string>& uops_events) {
        void* func = platform::arch::generate_uops_cache_codegenerate(instr_cnt, 
                                                                      config_.iterations, 
                                                                      {config_.instr.instr_type});
        if (!func) {
            SPDLOG_ERROR("[{}] failed to generate test function for instr_cnt={}", name(), instr_cnt);
            return {};
        }

        auto f = reinterpret_cast<void(*)()>(func);
        for (size_t i = 0; i < kDefaultWarmupIterations; ++i) f();
        
        std::vector<double> ticks_per_instr;
        std::vector<uint64_t> raw_ticks;
        std::vector<std::vector<uint64_t>> all_counts;

        for (size_t r = 0; r < config_.repeats; ++r) {
            if (pmc) {
                pmc->reset();
                pmc->enable();
            }

            uint64_t start_ticks = platform::arch::tick();
            f();
            uint64_t end_ticks = platform::arch::tick();

            if (pmc) pmc->disable();

            uint64_t ticks = end_ticks - start_ticks;
            raw_ticks.push_back(ticks);
            ticks_per_instr.push_back(static_cast<double>(ticks) / instr_cnt);

            if (pmc) {
                auto cv = pmc->read();
                all_counts.push_back(std::move(cv.values));
            }
        }

        double avg_ticks_per_instr = std::accumulate(ticks_per_instr.begin(), ticks_per_instr.end(), 0.0) / config_.repeats;
        double avg_raw_ticks = std::accumulate(raw_ticks.begin(), raw_ticks.end(), 0.0) / config_.repeats;

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

        SPDLOG_INFO("[{}] instr_cnt={}: avg_ticks_per_instr={:.4g} (std={:.4g})", name(), instr_cnt, avg_ticks_per_instr, ticks_std);
        if (!avg_events_counts.empty()) {
            for (size_t i = 0; i < uops_events.size(); ++i) {
                SPDLOG_INFO("  {} avg = {:.4g}", uops_events[i], static_cast<double>(avg_events_counts[i]));
            }
            if (uops_events.size() >= 2) {
                double dsb = static_cast<double>(avg_events_counts[1]);
                double mite = static_cast<double>(avg_events_counts[0]);
                SPDLOG_INFO(" uops_cache share to all = {:.4g}", dsb / (dsb + mite));
            }
        }

        return {avg_ticks_per_instr, avg_raw_ticks, ticks_std, std::move(avg_events_counts)};
    }

    // Find approximate saturation point by locating the largest drop in DSB share
    size_t findApproxSaturation(const std::vector<size_t>& counts,
                                const std::vector<UopsCacheResult>& results,
                                const std::vector<std::string>& uops_events) {
        if (counts.size() < 3) return 0;

        size_t dsb_idx = std::string::npos, mite_idx = std::string::npos;
        for (size_t i = 0; i < uops_events.size(); ++i) {
            if (uops_events[i].find("dsb_uops") != std::string::npos) dsb_idx = i;
            if (uops_events[i].find("mite_uops") != std::string::npos) mite_idx = i;
        }
        if (dsb_idx == std::string::npos || mite_idx == std::string::npos) return 0;

        std::vector<double> dsb_share;
        for (const auto& r : results) {
            if (r.avg_events_counts.empty()) return 0;
            uint64_t dsb = r.avg_events_counts[dsb_idx];
            uint64_t mite = r.avg_events_counts[mite_idx];
            double share = (dsb + mite) > 0 ? static_cast<double>(dsb) / (dsb + mite) : 0.0;
            dsb_share.push_back(share);
        }

        // Find the largest drop between consecutive points, ignoring first few
        double max_drop = 0.0;
        size_t drop_idx = 0;
        for (size_t i = kCoarseIgnoreFirst; i < dsb_share.size(); ++i) {
            double drop = dsb_share[i-1] - dsb_share[i];
            if (drop > max_drop) {
                max_drop = drop;
                drop_idx = i;   // i is the index where share becomes low
            }
        }

        if (max_drop > kDSBDropSignificant) {
            // Return the instruction count at the point before the drop
            return counts[drop_idx - 1];
        }
        return 0;
    }

    // Binary search for the largest N where DSB share >= kDSBShareRefine
    size_t refineSaturation(size_t approx, platform::pmc::PmcGroup* pmc,
                            const std::vector<std::string>& uops_events) {
        size_t left = (approx > config_.instr_step) ? approx - config_.instr_step : config_.min_instr_cnt;
        size_t right = approx + config_.instr_step;
        if (right > config_.max_instr_cnt) right = config_.max_instr_cnt;

        size_t dsb_idx = std::string::npos, mite_idx = std::string::npos;
        for (size_t i = 0; i < uops_events.size(); ++i) {
            if (uops_events[i].find("dsb_uops") != std::string::npos) dsb_idx = i;
            if (uops_events[i].find("mite_uops") != std::string::npos) mite_idx = i;
        }
        if (dsb_idx == std::string::npos || mite_idx == std::string::npos) return approx;

        size_t answer = left;
        while (left <= right) {
            size_t mid = left + (right - left) / 2;
            UopsCacheResult res = run_test(mid, pmc, uops_events);
            if (res.avg_events_counts.empty()) break;

            uint64_t dsb = res.avg_events_counts[dsb_idx];
            uint64_t mite = res.avg_events_counts[mite_idx];
            double share = (dsb + mite) > 0 ? static_cast<double>(dsb) / (dsb + mite) : 0.0;

            if (share >= kDSBShareRefine) {
                answer = mid;
                left = mid + 1;
            } else {
                right = mid - 1;
            }
        }
        return answer;
    }

    Config config_;
};

} // namespace silicon_probe::uops_cache
