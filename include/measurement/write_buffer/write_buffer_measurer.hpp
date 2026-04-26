// write_buffer_measurer.hpp
#pragma once

#include "infra/logging.hpp"
#include "measurement/core/measurer.hpp"
#include "platform/arch.hpp"
#include "platform/pmc.hpp"
#include "platform/os.hpp"
#include "platform/events_discovery.hpp"
#include <memory>
#include <numeric>
#include <cmath>
#include <cstring>

namespace silicon_probe::write_buffer {

struct WriteBufferResult {
    double avg_latency_ticks;
    double latency_stddev;
    std::vector<uint64_t> avg_events;
};

class WriteBufferMeasurer final : public core::Measurer {
public:
    static constexpr size_t kDefaultMaxWrites = 128;
    static constexpr size_t kDefaultMinWrites = 1;
    static constexpr size_t kDefaultWritesStep = 1;
    static constexpr size_t kDefaultIterations = 1000;
    static constexpr size_t kDefaultRepeats = 30;
    static constexpr size_t kDefaultWarmupIterations = 5;
    static constexpr size_t kBufferSizeMB = 16;
    static constexpr size_t kBytesPerEntry = 4;
    static constexpr size_t kCacheLineSize = 64;

    struct Config {
        bool enabled = true;
        platform::MeasurementEnvironmentOptions environment;
        size_t max_writes = kDefaultMaxWrites;
        size_t min_writes = kDefaultMinWrites;
        size_t writes_step = kDefaultWritesStep;
        size_t iterations = kDefaultIterations;
        size_t repeats = kDefaultRepeats;
        size_t warmup_iterations = kDefaultWarmupIterations;
        double latency_growth_ratio = 2.0;
        double pmc_saturation_ratio = 0.1;
    };

    WriteBufferMeasurer() : WriteBufferMeasurer(Config{}) {}
    explicit WriteBufferMeasurer(Config config) : config_(std::move(config)) {
        SPDLOG_INFO("[{}] cfg: min_writes={} max_writes={} step={} samples_per_repeat={} repeats={}",
                    name(), config_.min_writes, config_.max_writes, config_.writes_step,
                    config_.iterations, config_.repeats);
    }

    std::string_view name() const noexcept override { return "write_buffer"; }

    void measure(core::CpuInfoData& data) override {
        SPDLOG_INFO("[{}] start", name());
        platform::ScopedMeasurementEnvironment env{config_.environment};

        auto events = platform::discover_write_buffer_events();
        bool has_pmc = !events.empty();
        std::unique_ptr<platform::pmc::PmcGroup> pmc;
        size_t sb_idx = std::string::npos, bound_idx = std::string::npos;
        if (has_pmc) {
            pmc = platform::pmc::PmcGroup::create_raw(events);
            if (!pmc) {
                SPDLOG_WARN("[{}] PMC open failed, fallback to time-only", name());
                has_pmc = false;
            } else {
                for (size_t i = 0; i < events.size(); ++i) {
                    if (events[i].find("resource_stalls.sb") != std::string::npos) sb_idx = i;
                    if (events[i].find("exe_activity.bound_on_stores") != std::string::npos) bound_idx = i;
                }
            }
        }

        const size_t buffer_bytes = kBufferSizeMB * 1024 * 1024;
        const size_t num_elements = buffer_bytes / kBytesPerEntry;
        std::unique_ptr<int[]> fill_area(new int[num_elements]);
        std::unique_ptr<int[]> extra_area(new int[num_elements]);
        std::memset(fill_area.get(), 0, buffer_bytes);
        std::memset(extra_area.get(), 0, buffer_bytes);

        const size_t region_size = ((config_.max_writes * kCacheLineSize) + kCacheLineSize - 1) & ~(kCacheLineSize - 1);
        if ((config_.max_writes + 1) * region_size > num_elements * kBytesPerEntry) {
            SPDLOG_ERROR("[{}] Buffer too small for requested max_writes", name());
            return;
        }

        volatile int dummy = 0;
        SPDLOG_INFO("| writes | latency(ticks) | stddev |");
        if (has_pmc) {
            for (const auto& ev : events)
                SPDLOG_INFO("|        | {} (per sample) |", ev);
        }

        std::vector<size_t> writes_list;
        std::vector<WriteBufferResult> results;

        for (size_t num_writes = config_.min_writes; num_writes <= config_.max_writes;
             num_writes += config_.writes_step) {
            size_t region_offset = (num_writes - 1) * region_size / kBytesPerEntry;
            int* fill_ptr = fill_area.get() + region_offset;
            volatile int* extra_ptr = extra_area.get() + region_offset;

            writes_list.push_back(num_writes);
            WriteBufferResult res = measure_for_writes(num_writes, fill_ptr, extra_ptr, dummy, pmc.get());
            results.push_back(res);

            if (has_pmc) {
                SPDLOG_INFO("| {:6} | {:12.2f} | {:6.2f} |", num_writes, res.avg_latency_ticks, res.latency_stddev);
                for (size_t i = 0; i < events.size(); ++i) {
                    double per_sample = (res.avg_events.size() > i) ? double(res.avg_events[i]) / config_.iterations : 0.0;
                    double total = (res.avg_events.size() > i) ? double(res.avg_events[i]) : 0.0;
                    SPDLOG_INFO("|        | {}: {:.2f} per sample (total {}) |", events[i], per_sample, total);
                }
            }
        }

        if (results.size() >= 2) {
            size_t capacity = analyze_buffer_capacity(results, writes_list, has_pmc, sb_idx, bound_idx);
            SPDLOG_INFO("[{}] Estimated write buffer capacity: {} entries (each 4 bytes)", name(), capacity);
            data.write_buffer_size = capacity;
        }
    }

private:
    Config config_;

    WriteBufferResult measure_for_writes(size_t num_writes, int* fill_base,
                                         volatile int* extra_addr, volatile int& dummy,
                                         platform::pmc::PmcGroup* pmc) {
        const size_t stride = kCacheLineSize / kBytesPerEntry;

        // warmup
        for (size_t w = 0; w < config_.warmup_iterations; ++w) {
            for (size_t i = 0; i < num_writes; ++i) {
                fill_base[i * stride] = static_cast<int>(i);   // regular store
            }
            dummy = *extra_addr;
            platform::arch::lfence();
        }

        std::vector<double> time_samples;
        std::vector<std::vector<uint64_t>> pmc_samples;
        time_samples.reserve(config_.repeats);
        if (pmc) pmc_samples.reserve(config_.repeats);

        for (size_t r = 0; r < config_.repeats; ++r) {
            if (pmc) {
                pmc->reset();
                pmc->enable();
            }

            uint64_t total_ticks = 0;
            for (size_t iter = 0; iter < config_.iterations; ++iter) {
                // 1. Сбросить все линии, в которые будем писать
                for (size_t i = 0; i < num_writes; ++i) {
                    platform::arch::clflush(&fill_base[i * stride]);
                }
                platform::arch::flush_complete(); // mfence – дождаться завершения clflush

                // 2. Заполнить буфер записи
                for (size_t i = 0; i < num_writes; ++i) {
                    fill_base[i * stride] = static_cast<int>(iter + i);
                }

                // 3. Измерить критическую пару store+load
                uint64_t start = platform::arch::tick();
                const_cast<int*>(extra_addr)[0] = 0xdeadbeef;
                dummy = *extra_addr;
                uint64_t end = platform::arch::tick();
                total_ticks += (end - start);
                platform::arch::lfence();
            }

            if (pmc) {
                pmc->disable();
                auto cv = pmc->read();
                if (cv.valid) pmc_samples.push_back(std::move(cv.values));
            }

            double avg_ticks = static_cast<double>(total_ticks) / config_.iterations;
            time_samples.push_back(avg_ticks);
        }

        double avg = std::accumulate(time_samples.begin(), time_samples.end(), 0.0) / config_.repeats;
        double stddev = 0.0;
        for (double v : time_samples) {
            double d = v - avg;
            stddev += d * d;
        }
        stddev = std::sqrt(stddev / config_.repeats);

        std::vector<uint64_t> avg_events;
        if (pmc && !pmc_samples.empty()) {
            avg_events.assign(pmc_samples[0].size(), 0);
            for (const auto& sample : pmc_samples)
                for (size_t i = 0; i < sample.size(); ++i)
                    avg_events[i] += sample[i];
            for (size_t i = 0; i < avg_events.size(); ++i)
                avg_events[i] /= config_.repeats;
        }
        if (pmc) {
            SPDLOG_DEBUG("[{}] num_writes={}, samples: ticks={:.2f}+-{:.2f}", 
                         name(), num_writes, avg, stddev);
            for (size_t i = 0; i < avg_events.size(); ++i) {
                SPDLOG_DEBUG("[{}]   event{} = {} total, {:.2f} per iter", 
                             name(), i, avg_events[i], double(avg_events[i]) / config_.iterations);
            }
        }
        return {avg, stddev, std::move(avg_events)};
    }

    size_t analyze_buffer_capacity(const std::vector<WriteBufferResult>& results,
                                   const std::vector<size_t>& writes_list,
                                   bool has_pmc, size_t sb_idx, size_t bound_idx) {
        double base_latency = results[0].avg_latency_ticks;
        size_t max_working = 0;

        for (size_t i = 0; i < results.size(); ++i) {
            double latency_ratio = results[i].avg_latency_ticks / base_latency;
            bool time_ok = (latency_ratio < config_.latency_growth_ratio);

            bool stalls_ok = true;
            if (has_pmc && i < results.size()) {
                double stalls_per_sample = 0.0;
                if (sb_idx != std::string::npos && sb_idx < results[i].avg_events.size())
                    stalls_per_sample = double(results[i].avg_events[sb_idx]) / config_.iterations;
                else if (bound_idx != std::string::npos && bound_idx < results[i].avg_events.size())
                    stalls_per_sample = double(results[i].avg_events[bound_idx]) / config_.iterations;

                if (stalls_per_sample > config_.pmc_saturation_ratio)
                    stalls_ok = false;
            }

            bool is_working = (has_pmc) ? (time_ok && stalls_ok) : time_ok;

            if (is_working)
                max_working = writes_list[i];
            else
                break;
        }
        return max_working;
    }
};

} // namespace silicon_probe::write_buffer
