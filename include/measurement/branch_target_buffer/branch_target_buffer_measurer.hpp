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

namespace silicon_probe::branch_target_buffer {

struct InstructionData {
    platform::arch::InstrType instr_type;
    std::string instr_name;
};

struct BranchTargetBufferResult {
    double avg_ticks_per_block;
    double avg_ticks_per_iter;
    double ticks_std;
    uint64_t avg_events_counts;
};

class BranchTargetBufferMeasurer final : public core::Measurer {
  public:
    static constexpr size_t kDefaultMinBlocksCnt = 3500;
    static constexpr size_t kDefaultMaxBlocksCnt = 5000;
    static constexpr size_t kDefaultBlocksStep = 100;
    static constexpr size_t kDefaultIterations = 100'000;
    static constexpr size_t kDefaultRepeats = 10;
    static constexpr size_t kDefaultWarmupIterations = 100;
    static constexpr size_t kDefaultAlignment = 16;

    struct Config {
        bool enabled = true;
        platform::MeasurementEnvironmentOptions environment;
        size_t min_blocks_cnt = kDefaultMinBlocksCnt;
        size_t max_blocks_cnt = kDefaultMaxBlocksCnt;
        size_t blocks_step = kDefaultBlocksStep;
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

    BranchTargetBufferMeasurer() : BranchTargetBufferMeasurer(Config{}) {}
    explicit BranchTargetBufferMeasurer(Config config) : config_(std::move(config)) {
        SPDLOG_INFO("[{}] configured: min_blocks_cnt = {}, max_blocks_cnt = {}, blocks_step = {}, iterations={}, "
                    "repeats={}, alignment={}",
                    name(), config_.min_blocks_cnt, config_.max_blocks_cnt, config_.blocks_step, config_.iterations, config_.repeats,
                    config_.alignment);
    }

    std::string_view name() const noexcept override { return "branch target buffer"; }

    void measure(core::CpuInfoData& data) override {
        SPDLOG_INFO("[{}] starting BTB size measurement", name());

        platform::ScopedMeasurementEnvironment environment{config_.environment};

        std::optional<std::string> btb_event = platform::discover_branch_target_buffer_events();
        bool use_events = btb_event.has_value();
        std::unique_ptr<platform::pmc::PmcGroup> pmc;

        if (use_events) {
            SPDLOG_INFO("[{}] Found BTB event: {}", name(), *btb_event);
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
        size_t first_bad_point = 0;

        for (size_t blocks_cnt = config_.min_blocks_cnt; blocks_cnt <= config_.max_blocks_cnt; blocks_cnt += config_.blocks_step) {
            BranchTargetBufferResult res = run_test(blocks_cnt, pmc.get());
            coarse_counts.push_back(blocks_cnt);
            coarse_results.push_back(res);

            if (use_events && !saturation_detected) {
                double rate = computeMispredictionRate(res, blocks_cnt);
                if (rate > config_.misprediction_saturation_threshold) {
                    SPDLOG_INFO("[{}] Saturation detected (misprediction rate = {:.4f} > {:.4f}) at blocks_cnt = {}", name(), rate,
                                config_.misprediction_saturation_threshold, blocks_cnt);
                    saturation_detected = true;
                    first_bad_point = blocks_cnt;
                    break;
                }
            }
        }

        if (use_events && !saturation_detected && coarse_counts.size() >= 3) {
            SPDLOG_WARN("[{}] Saturation not reached within the range, using last points.", name());
        }

        size_t approx_saturation = findApproxSaturation(coarse_counts, coarse_results, use_events);
        if (approx_saturation == 0) {
            platform::arch::release_branch_target_buffer_code();
            data.btb_size = 0;
            SPDLOG_WARN("[{}] Could not detect saturation point.", name());
            return;
        }

        SPDLOG_INFO("[{}] Starting to refine saturation", name());
        size_t refined = 0;
        if (use_events) {
            refined = refineSaturation(approx_saturation, first_bad_point, pmc.get());
        } else {
            refined = refineSaturationTime(approx_saturation, pmc.get());
        }

        platform::arch::release_branch_target_buffer_code();
        data.btb_size = refined;
        SPDLOG_INFO("[{}] Estimated BTB size: {} entries", name(), refined);
        SPDLOG_INFO("[{}] Measurement complete", name());
    }

  private:
    Config config_;

    BranchTargetBufferResult run_test(size_t blocks_cnt, platform::pmc::PmcGroup* pmc) {
        std::vector<double> ticks_per_block;
        ticks_per_block.reserve(config_.repeats);
        std::vector<uint64_t> raw_ticks;
        raw_ticks.reserve(config_.repeats);
        std::vector<uint64_t> all_counts;
        all_counts.reserve(config_.repeats);

        auto funcs = platform::arch::generate_branch_target_buffer_code(blocks_cnt, config_.iterations, config_.alignment);
        if (funcs.size() < 2 || !funcs[0] || !funcs[1]) {
            SPDLOG_ERROR("[{}] Failed to generate functions for blocks_cnt={}", name(), blocks_cnt);
            return {};
        }

        auto warmup_func = reinterpret_cast<void (*)()>(funcs[0]);
        auto measure_func = reinterpret_cast<void (*)()>(funcs[1]);

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
            raw_ticks.push_back(ticks);
            ticks_per_block.push_back(static_cast<double>(ticks) / blocks_cnt);

            if (pmc) {
                auto cv = pmc->read();
                all_counts.push_back(cv.values[0]);
            }
        }

        double avg_ticks_per_block = std::accumulate(ticks_per_block.begin(), ticks_per_block.end(), 0.0) / config_.repeats;
        double avg_raw_ticks = std::accumulate(raw_ticks.begin(), raw_ticks.end(), 0.0) / config_.repeats;

        double ticks_std = 0.0;
        for (double t : ticks_per_block) {
            double diff = t - avg_ticks_per_block;
            ticks_std += diff * diff;
        }
        ticks_std = std::sqrt(ticks_std / config_.repeats);

        uint64_t avg_events_counts = 0;
        if (!all_counts.empty()) {
            for (uint64_t cnt : all_counts) {
                avg_events_counts += cnt;
            }
            avg_events_counts /= config_.repeats;
        }

        if (pmc) {
            double rate = static_cast<double>(avg_events_counts) / (blocks_cnt * config_.iterations);
            SPDLOG_INFO("[{}]\nblocks_cnt={}: avg_ticks_per_block={:.4e} (std={:.4e}), misprediction_rate={:.4f}", name(), blocks_cnt,
                        avg_ticks_per_block, ticks_std, rate);
        } else {
            SPDLOG_INFO("[{}]\nblocks_cnt={}: avg_ticks_per_block={:.4e} (std={:.4e})", name(), blocks_cnt, avg_ticks_per_block, ticks_std);
        }

        return {avg_ticks_per_block, avg_raw_ticks, ticks_std, avg_events_counts};
    }

    double computeMispredictionRate(const BranchTargetBufferResult& res, size_t blocks_cnt) const {
        uint64_t total_branches = static_cast<uint64_t>(blocks_cnt) * config_.iterations;
        if (total_branches == 0) return 0.0;
        return static_cast<double>(res.avg_events_counts) / total_branches;
    }

    double computeTimePerBlock(const BranchTargetBufferResult& res) const { return res.avg_ticks_per_block; }

    size_t findApproxSaturation(const std::vector<size_t>& counts, const std::vector<BranchTargetBufferResult>& results, bool use_events) {
        if (counts.size() < 4) return 0;

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
            for (size_t i = config_.coarse_ignore_first; i < results.size(); ++i) {
                double cur_rate = computeMispredictionRate(results[i], counts[i]);
                double prev_rate = computeMispredictionRate(results[i - 1], counts[i - 1]);
                double growth = cur_rate - prev_rate;
                if (growth > max_growth) {
                    max_growth = growth;
                    growth_idx = i;
                }
            }
            if (max_growth > config_.misprediction_growth_threshold && growth_idx > 0) {
                return counts[growth_idx - 1];
            }
            return 0;
        } else {
            // Time-based: find step with baseline comparison
            std::vector<double> times;
            for (size_t i = 0; i < counts.size(); ++i) {
                times.push_back(results[i].avg_ticks_per_block);
            }

            // Baseline from first 3 points
            double baseline = 0.0;
            size_t baseline_cnt = std::min<size_t>(3, times.size());
            for (size_t i = 0; i < baseline_cnt; ++i) {
                baseline += times[i];
            }
            baseline /= baseline_cnt;

            // Find first index where time exceeds baseline * growth ratio
            size_t jump_idx = 0;
            for (size_t i = config_.coarse_ignore_first; i < times.size(); ++i) {
                if (times[i] > baseline * config_.time_growth_ratio) {
                    jump_idx = i;
                    break;
                }
            }
            if (jump_idx == 0) return 0;

            // Verify stability: next points also exceed threshold
            size_t stable = 0;
            for (size_t i = jump_idx; i < std::min(times.size(), jump_idx + config_.time_stability_points); ++i) {
                if (times[i] > baseline * config_.time_growth_ratio) {
                    stable++;
                }
            }
            if (stable >= config_.time_stability_points) {
                return counts[jump_idx - 1];
            }
            return 0;
        }
    }

    // Event-based refinement with binary search between good and bad points
    size_t refineSaturation(size_t good, size_t bad, platform::pmc::PmcGroup* pmc) {
        size_t left = good;
        size_t right = bad;
        size_t answer = left;

        while (left + 1 < right) {
            size_t mid = left + (right - left) / 2;
            BranchTargetBufferResult res = run_test(mid, pmc);
            double rate = computeMispredictionRate(res, mid);

            if (rate <= config_.misprediction_saturation_threshold) {
                answer = mid;
                left = mid;
            } else {
                right = mid;
            }
        }
        return answer;
    }

    // Time-based refinement using baseline and binary search
    size_t refineSaturationTime(size_t approx, platform::pmc::PmcGroup* pmc) {
        // Compute baseline from first few block counts
        const size_t baseline_samples = 3;
        double baseline = 0.0;
        size_t baseline_cnt = 0;
        for (size_t i = 0; i < baseline_samples; ++i) {
            size_t test_cnt = config_.min_blocks_cnt + i * config_.blocks_step;
            if (test_cnt > config_.max_blocks_cnt) break;
            BranchTargetBufferResult res = run_test(test_cnt, pmc);
            baseline += res.avg_ticks_per_block;
            baseline_cnt++;
        }
        if (baseline_cnt == 0) return approx;
        baseline /= baseline_cnt;

        // Determine search range around approx
        size_t left = (approx > config_.blocks_step) ? approx - config_.blocks_step : config_.min_blocks_cnt;
        size_t right = approx + config_.blocks_step;
        left = (left > config_.blocks_step) ? left - config_.blocks_step : config_.min_blocks_cnt;
        right = right + config_.blocks_step;
        if (right > config_.max_blocks_cnt) right = config_.max_blocks_cnt;

        size_t answer = left;
        while (left <= right) {
            size_t mid = left + (right - left) / 2;
            BranchTargetBufferResult res = run_test(mid, pmc);

            if (res.avg_ticks_per_block <= baseline * config_.time_growth_ratio) {
                answer = mid;
                left = mid + 1;
            } else {
                right = mid - 1;
            }
        }
        return answer;
    }
};

} // namespace silicon_probe::branch_target_buffer
