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
    static constexpr size_t kDefaultMaxWrites = 256;
    static constexpr size_t kDefaultMinWrites = 1;
    static constexpr size_t kDefaultWritesStep = 1;
    static constexpr size_t kDefaultIterations = 100'000;
    static constexpr size_t kDefaultRepeats = 10;
    static constexpr size_t kDefaultWarmupIterations = 10;

    struct Config {
        bool enabled = true;
        platform::MeasurementEnvironmentOptions environment;
        size_t max_writes = kDefaultMaxWrites;
        size_t min_writes = kDefaultMinWrites;
        size_t writes_step = kDefaultWritesStep;
        size_t iterations = kDefaultIterations;
        size_t repeats = kDefaultRepeats;
        size_t warmup_iterations = kDefaultWarmupIterations;
        double latency_growth_ratio = 1.5;
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
        size_t sb_idx = std::string::npos;    // resource_stalls.sb
        size_t bound_idx = std::string::npos; // exe_activity.bound_on_stores

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

        // Allocate write area (for stores) and separate read area (independent address)
        const size_t write_entries = config_.max_writes + 16;
        std::unique_ptr<int[]> write_area(new int[write_entries]);
        std::unique_ptr<int[]> read_area(new int[64]);   // independent buffer
        std::memset(write_area.get(), 0, sizeof(int) * write_entries);
        std::memset(read_area.get(), 0, sizeof(int) * 64);

        volatile int* read_addr = read_area.get();
        volatile int dummy = 0;

        // Print header for results table
        if (has_pmc) {
            SPDLOG_INFO("| writes | latency(ticks) | stddev |");
            for (const auto& ev : events) {
                SPDLOG_INFO("|        | {} (per sample) |", ev);
            }
        } else {
            SPDLOG_INFO("| writes | latency(ticks) | stddev |");
        }

        std::vector<size_t> writes_list;
        std::vector<WriteBufferResult> results;

        for (size_t num_writes = config_.min_writes; num_writes <= config_.max_writes;
             num_writes += config_.writes_step) {
            writes_list.push_back(num_writes);
            WriteBufferResult res = measure_for_writes(num_writes, write_area.get(),
                                                       read_addr, dummy, pmc.get());
            results.push_back(res);

            if (has_pmc) {
                SPDLOG_INFO("| {:6} | {:12.2f} | {:6.2f} |", num_writes, res.avg_latency_ticks, res.latency_stddev);
                for (size_t i = 0; i < events.size(); ++i) {
                    double per_sample = (res.avg_events.size() > i) ? double(res.avg_events[i]) / config_.iterations : 0.0;
                    SPDLOG_INFO("|        | {:20.2f} |", per_sample);
                }
            } else {
                SPDLOG_INFO("| {:6} | {:12.2f} | {:6.2f} |", num_writes, res.avg_latency_ticks, res.latency_stddev);
            }
        }

        if (results.size() < 2) {
            SPDLOG_WARN("[{}] Not enough data points", name());
            return;
        }

        size_t max_working_writes = analyze_buffer_capacity(results, writes_list,
                                                            has_pmc, sb_idx, bound_idx);
        SPDLOG_INFO("[{}] Estimated write buffer capacity: {} entries (each 4 bytes)", name(), max_working_writes);
        data.write_buffer_size = max_working_writes;
    }

private:
    Config config_;

    WriteBufferResult measure_for_writes(size_t num_writes, int* write_area,
                                         volatile int* read_addr, volatile int& dummy,
                                         platform::pmc::PmcGroup* pmc) {
        platform::arch::clflush(const_cast<int*>(read_addr));
        asm volatile("" : : : "memory");

        // Warmup
        for (size_t w = 0; w < config_.warmup_iterations; ++w) {
            for (size_t i = 0; i < num_writes; ++i) {
                write_area[i] = static_cast<int>(i);   // ordinary store
            }
            dummy = *read_addr;   // load from independent address
            asm volatile("" : : : "memory");
        }

        std::vector<double> latency_samples;
        std::vector<std::vector<uint64_t>> pmc_samples;
        latency_samples.reserve(config_.repeats);
        if (pmc) pmc_samples.reserve(config_.repeats);

        for (size_t r = 0; r < config_.repeats; ++r) {
            if (pmc) {
                pmc->reset();
                pmc->enable();
            }

            uint64_t total_read_ticks = 0;
            for (size_t iter = 0; iter < config_.iterations; ++iter) {
                // Fill store buffer with ordinary stores
                for (size_t i = 0; i < num_writes; ++i) {
                    write_area[i] = static_cast<int>(iter + i);
                }
                // Measure load latency from independent address
                uint64_t start = platform::arch::tick();
                dummy = *read_addr;
                uint64_t end = platform::arch::tick();
                total_read_ticks += (end - start);
                asm volatile("" : : : "memory");
            }

            if (pmc) {
                pmc->disable();
                auto cv = pmc->read();
                if (cv.valid) pmc_samples.push_back(std::move(cv.values));
            }

            double avg_latency = static_cast<double>(total_read_ticks) / config_.iterations;
            latency_samples.push_back(avg_latency);
        }

        double avg_latency = std::accumulate(latency_samples.begin(), latency_samples.end(), 0.0) / config_.repeats;
        double stddev = 0.0;
        for (double v : latency_samples) {
            double d = v - avg_latency;
            stddev += d * d;
        }
        stddev = std::sqrt(stddev / config_.repeats);

        std::vector<uint64_t> avg_events;
        if (pmc && !pmc_samples.empty()) {
            avg_events.assign(pmc_samples[0].size(), 0);
            for (const auto& sample : pmc_samples) {
                for (size_t i = 0; i < sample.size(); ++i)
                    avg_events[i] += sample[i];
            }
            for (size_t i = 0; i < avg_events.size(); ++i)
                avg_events[i] /= config_.repeats;
        }

        return {avg_latency, stddev, std::move(avg_events)};
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
