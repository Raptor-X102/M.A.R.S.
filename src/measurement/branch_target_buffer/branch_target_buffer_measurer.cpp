#include "measurement/branch_target_buffer/branch_target_buffer_measurer.hpp"

namespace silicon_probe::branch_target_buffer {

BranchTargetBufferMeasurer::BranchTargetBufferMeasurer() : BranchTargetBufferMeasurer(Config{}) {}

BranchTargetBufferMeasurer::BranchTargetBufferMeasurer(Config config) : config_(std::move(config)) {
    validateConfig();
    SPDLOG_DEBUG(
        "[{}] configured: min_blocks_cnt = {}, max_blocks_cnt = {}, blocks_step = {}, iterations={}, "
        "repeats={}, alignment={}",
        name(),
        config_.min_blocks_cnt,
        config_.max_blocks_cnt,
        config_.blocks_step,
        config_.iterations,
        config_.repeats,
        config_.alignment
    );
}

std::string_view BranchTargetBufferMeasurer::name() const noexcept { return "branch target buffer"; }

void BranchTargetBufferMeasurer::validateConfig() {
    // coarse_ignore_first must be >= 1 to avoid index -1
    if (config_.coarse_ignore_first == 0) {
        SPDLOG_WARN("[{}] coarse_ignore_first = 0 is invalid, setting to 1", name());
        config_.coarse_ignore_first = 1;
    }

    // min_blocks_cnt cannot be zero (division by zero in computeMispredictionRate)
    if (config_.min_blocks_cnt == 0) {
        SPDLOG_WARN("[{}] min_blocks_cnt = 0 is invalid, setting to {}", name(), kDefaultMinBlocksCnt);
        config_.min_blocks_cnt = kDefaultMinBlocksCnt;
    }

    // blocks_step cannot be zero (infinite loop)
    if (config_.blocks_step == 0) {
        SPDLOG_WARN("[{}] blocks_step = 0 is invalid, setting to {}", name(), kDefaultBlocksStep);
        config_.blocks_step = kDefaultBlocksStep;
    }

    // repeats cannot be zero (division by zero in averaging)
    if (config_.repeats == 0) {
        SPDLOG_WARN("[{}] repeats = 0 is invalid, setting to {}", name(), kDefaultRepeats);
        config_.repeats = kDefaultRepeats;
    }

    // warmup_iterations cannot be zero (need warmup)
    if (config_.warmup_iterations == 0) {
        SPDLOG_WARN("[{}] warmup_iterations = 0 is invalid, setting to {}", name(), kDefaultWarmupIterations);
        config_.warmup_iterations = kDefaultWarmupIterations;
    }

    // iterations cannot be zero (no branches to measure)
    if (config_.iterations == 0) {
        SPDLOG_WARN("[{}] iterations = 0 is invalid, setting to {}", name(), kDefaultIterations);
        config_.iterations = kDefaultIterations;
    }

    // misprediction_saturation_threshold must be in (0, 1]
    if (config_.misprediction_saturation_threshold <= 0.0 || 
        config_.misprediction_saturation_threshold > 1.0) {
        SPDLOG_WARN("[{}] misprediction_saturation_threshold = {} is invalid, setting to 0.01", 
                    name(), config_.misprediction_saturation_threshold);
        config_.misprediction_saturation_threshold = 0.01;
    }

    // time_growth_ratio must be > 1.0 (otherwise never triggers)
    if (config_.time_growth_ratio <= 1.0) {
        SPDLOG_WARN("[{}] time_growth_ratio = {} is invalid, setting to 1.2", 
                    name(), config_.time_growth_ratio);
        config_.time_growth_ratio = 1.2;
    }

    // time_stability_points cannot be zero
    if (config_.time_stability_points == 0) {
        SPDLOG_WARN("[{}] time_stability_points = 0 is invalid, setting to 3", name());
        config_.time_stability_points = 3;
    }

    // alignment must be positive
    if (config_.alignment < 1) {
        SPDLOG_WARN("[{}] alignment = {} is invalid, setting to {}", name(), config_.alignment, kDefaultAlignment);
        config_.alignment = kDefaultAlignment;
    }
}

struct CodeReleaseGuard {
    ~CodeReleaseGuard() { platform::arch::release_branch_target_buffer_code(); }
};

void BranchTargetBufferMeasurer::measure(shared_types::CpuInfoData& data) {
    SPDLOG_INFO("[{}] starting BTB size measurement", name());

    platform::ScopedMeasurementEnvironment environment{config_.environment};

    std::optional<std::string> btb_event = platform::discover_branch_target_buffer_events(data);
    bool use_events                      = btb_event.has_value();
    std::unique_ptr<platform::pmc::PmcGroup> pmc;

    if (use_events) {
        SPDLOG_DEBUG("[{}] Found BTB event: {}", name(), *btb_event);
        pmc = platform::pmc::PmcGroup::create_raw({*btb_event});
        if (!pmc) {
            SPDLOG_WARN("[{}] Failed to open BTB counter, falling back to time-based measurement.", name());
            use_events = false;
        }
    } else {
        SPDLOG_WARN("[{}] No BTB misprediction event found. Using time-based measurement.", name());
    }

    // Coarse scan
    std::vector<BranchTargetBufferResult> coarse_results;
    std::vector<size_t> coarse_counts;
    bool saturation_detected = false;
    size_t first_bad_point   = 0;

    // it is needed to release bht code that was generated in generate_branch_target_buffer_code 
    // after exception
    CodeReleaseGuard guard;

    for (size_t blocks_cnt = config_.min_blocks_cnt; blocks_cnt <= config_.max_blocks_cnt;
         blocks_cnt += config_.blocks_step) {
        BranchTargetBufferResult res = run_test(blocks_cnt, pmc.get());
        coarse_counts.push_back(blocks_cnt);
        coarse_results.push_back(res);

        if (use_events) {
            double rate = computeMispredictionRate(res, blocks_cnt);
            if (rate > config_.misprediction_saturation_threshold) {
                SPDLOG_DEBUG(
                    "[{}] Saturation detected (misprediction rate = {:.4f} > {:.4f}) at blocks_cnt = {}",
                    name(),
                    rate,
                    config_.misprediction_saturation_threshold,
                    blocks_cnt
                );
                saturation_detected = true;
                first_bad_point     = blocks_cnt;
                break;
            }
        }
    }

    if (use_events && !saturation_detected && coarse_counts.size() >= 3) {
        SPDLOG_WARN("[{}] Saturation not reached within the range, using last points.", name());
    }

    std::optional<size_t> approx_saturation;
    if (use_events && saturation_detected) {
        approx_saturation = first_bad_point - config_.blocks_step;
        if (approx_saturation < config_.min_blocks_cnt)
            approx_saturation = config_.min_blocks_cnt;
    } else {
        approx_saturation = findApproxSaturation(coarse_counts, coarse_results, use_events);
    }

    if (!approx_saturation) {
        return;
    }

    size_t refined = 0;
    if (use_events) {
        refined = refineSaturation(*approx_saturation, first_bad_point, pmc.get());
    } else {
        double baseline_time = 0.0;
        size_t baseline_cnt = std::min<size_t>(3, coarse_results.size());
        for (size_t i = 0; i < baseline_cnt; ++i)
            baseline_time += coarse_results[i].avg_ticks_per_block;
        baseline_time /= baseline_cnt;
        refined = refineSaturationTime(*approx_saturation, baseline_time, pmc.get());
    }

    data.btb_size = refined;
    SPDLOG_INFO("[{}] Estimated BTB size: {} entries", name(), refined);
    SPDLOG_INFO("[{}] Measurement complete", name());
}

BranchTargetBufferResult BranchTargetBufferMeasurer::run_test(size_t blocks_cnt, platform::pmc::PmcGroup* pmc) {
    std::vector<double> ticks_per_block(config_.repeats);
    std::vector<uint64_t> all_counts(config_.repeats);

    auto funcs = platform::arch::generate_branch_target_buffer_code(blocks_cnt, config_.iterations, config_.alignment);
    if (!funcs[0] || !funcs[1]) {
        SPDLOG_ERROR("[{}] Failed to generate functions for blocks_cnt={}", name(), blocks_cnt);
        return {};
    }

    auto warmup_func  = reinterpret_cast<void (*)()>(funcs[0]);
    auto measure_func = reinterpret_cast<void (*)()>(funcs[1]);

    // warmup
    for (size_t i = 0; i < config_.warmup_iterations; ++i) {
        warmup_func();
    }

    for (size_t r = 0; r < config_.repeats; ++r) {
        if (pmc) {
            pmc->reset();
            pmc->enable();
        }

        uint64_t start_ticks = platform::arch::tick();
        measure_func();
        uint64_t end_ticks = platform::arch::tick();

        if (pmc) {
            pmc->disable();
        }

        uint64_t ticks = end_ticks - start_ticks;
        ticks_per_block[r] = static_cast<double>(ticks) / blocks_cnt;

        if (pmc) {
            auto cv = pmc->read();
            all_counts[r] = cv.values[0];
        }
    }

    double avg_ticks_per_block = std::accumulate(ticks_per_block.begin(), ticks_per_block.end(), 0.0) / config_.repeats;

    // Counting Standard Deviation Sample
    double ticks_std = 0.0;
    for (double t : ticks_per_block) {
        double diff = t - avg_ticks_per_block;
        ticks_std += diff * diff;
    }
    ticks_std = std::sqrt(ticks_std / config_.repeats);

    // Averaging
    uint64_t avg_events_counts = 0;
    for (uint64_t cnt : all_counts) {
        avg_events_counts += cnt;
    }
    avg_events_counts /= config_.repeats;

    if (pmc) {
        double rate = static_cast<double>(avg_events_counts) / (blocks_cnt * config_.iterations);
        SPDLOG_DEBUG(
            "[{}]\nblocks_cnt={}: avg_ticks_per_block={:.4e} (std={:.4e}), misprediction_rate={:.4f}",
            name(),
            blocks_cnt,
            avg_ticks_per_block,
            ticks_std,
            rate
        );
    } else {
        SPDLOG_DEBUG(
            "[{}]\nblocks_cnt={}: avg_ticks_per_block={:.4e} (std={:.4e})",
            name(),
            blocks_cnt,
            avg_ticks_per_block,
            ticks_std
        );
    }

    return {avg_ticks_per_block, ticks_std, avg_events_counts};
}

double
BranchTargetBufferMeasurer::computeMispredictionRate(const BranchTargetBufferResult& res, size_t blocks_cnt) const {
    uint64_t total_branches = static_cast<uint64_t>(blocks_cnt) * config_.iterations;
    if (total_branches == 0)
        return 0.0;
    return static_cast<double>(res.avg_events_counts) / total_branches;
}

std::optional<size_t> BranchTargetBufferMeasurer::findApproxSaturation(
    const std::vector<size_t>& counts,
    const std::vector<BranchTargetBufferResult>& results,
    bool use_events
) {
    const int Min_res_cnt = 4;
    if (counts.size() < Min_res_cnt) 
        return std::nullopt;

    if (use_events) {
        // Event-based: find first index where rate exceeds threshold
        for (size_t i = config_.coarse_ignore_first; i < results.size(); ++i) {
            double rate = computeMispredictionRate(results[i], counts[i]);
            if (rate > config_.misprediction_saturation_threshold) {
                return counts[i - 1];
            }
        }

        // Fallback: max growth
        double max_growth = 0.0;
        size_t growth_idx = 0;
        if (results.size() <= config_.coarse_ignore_first) return std::nullopt;
        double prev_rate = computeMispredictionRate(results[config_.coarse_ignore_first - 1], 
                                                    counts [config_.coarse_ignore_first - 1]);

        for (size_t i = config_.coarse_ignore_first; i < results.size(); ++i) {
            double cur_rate  = computeMispredictionRate(results[i], counts[i]);
            double growth    = cur_rate - prev_rate;
            if (growth > max_growth) {
                max_growth = growth;
                growth_idx = i;
            }
            prev_rate = cur_rate; 
        }

        if (max_growth > config_.misprediction_growth_threshold && growth_idx > 0) {
            return counts[growth_idx - 1];
        }

        return std::nullopt;
    } else {
        // Time-based: find step with baseline comparison

        // Baseline from first 3 points
        double baseline = 0.0;
        size_t baseline_cnt = std::min<size_t>(3, results.size());
        for (size_t i = 0; i < baseline_cnt; ++i) {
            baseline += results[i].avg_ticks_per_block;
        }
        baseline /= baseline_cnt;

        // Find first index where time exceeds baseline * growth ratio
        size_t jump_idx = 0;
        for (size_t i = config_.coarse_ignore_first; i < results.size(); ++i) {
            if (results[i].avg_ticks_per_block > baseline * config_.time_growth_ratio) {
                jump_idx = i;
                break;
            }
        }
        if (jump_idx == 0)
            return std::nullopt;

        // Verify stability: next points also exceed threshold
        size_t stable = 0;
        size_t end_idx = std::min(results.size(), jump_idx + config_.time_stability_points);
        for (size_t i = jump_idx; i < end_idx; ++i) {
            if (results[i].avg_ticks_per_block > baseline * config_.time_growth_ratio) {
                stable++;
            }
        }
        if (stable >= config_.time_stability_points) {
            return counts[jump_idx - 1];
        }
        return std::nullopt;
    }
}

size_t BranchTargetBufferMeasurer::refineSaturation(size_t good, size_t bad, platform::pmc::PmcGroup* pmc) {
    size_t left   = good;
    size_t right  = bad;
    size_t answer = left;

    while (left + 1 < right) {
        size_t mid                   = left + (right - left) / 2;
        BranchTargetBufferResult res = run_test(mid, pmc);
        double rate                  = computeMispredictionRate(res, mid);

        if (rate <= config_.misprediction_saturation_threshold) {
            answer = mid;
            left   = mid;
        } else {
            right = mid;
        }
    }
    return answer;
}

size_t BranchTargetBufferMeasurer::refineSaturationTime(size_t approx, 
                                                        double baseline, 
                                                        platform::pmc::PmcGroup* pmc) {
    if (baseline <= 0.0) {
        SPDLOG_WARN("[{}] Invalid baseline value, falling back to approx", name());
        return approx;
    }

    size_t left  = (approx > config_.blocks_step) ? approx - config_.blocks_step : config_.min_blocks_cnt;
    size_t right = approx + config_.blocks_step;
    left         = (left > config_.blocks_step) ? left - config_.blocks_step : config_.min_blocks_cnt;
    right        = right + config_.blocks_step;
    if (right > config_.max_blocks_cnt)
        right = config_.max_blocks_cnt;

    size_t answer = left;
    while (left <= right) {
        size_t mid = left + (right - left) / 2;
        BranchTargetBufferResult res = run_test(mid, pmc);

        if (res.avg_ticks_per_block <= baseline * config_.time_growth_ratio) {
            answer = mid;
            left   = mid + 1;
        } else {
            right = mid - 1;
        }
    }
    return answer;
}

}  // namespace silicon_probe::branch_target_buffer
