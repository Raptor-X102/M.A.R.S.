// ExecPortsMeasurer.hpp
#pragma once

#include "silicon_probe/infra/logging.hpp"
#include "silicon_probe/core/measurer.hpp"
#include "silicon_probe/platform/arch.hpp"
#include "silicon_probe/platform/pmc.hpp"
#include "silicon_probe/platform/os.hpp"
#include "silicon_probe/platform/port_events_discovery.hpp"
#include <vector>
#include <string>
#include <algorithm>
#include <numeric>
#include <cmath>

namespace silicon_probe::exec_ports {

struct ExecPortsResult {
    std::string test_name;
    double avg_ticks;                 // average ticks per repeat
    double ticks_std;                 // standard deviation of ticks
    std::vector<uint64_t> avg_port_counts;
};

struct PortContentionDecision {
    bool different_ports;
    double confidence;
    std::string reasoning;
};

class ExecPortsMeasurer final : public core::Measurer {
public:
    static constexpr size_t kDefaultInstrCnt = 10000;
    static constexpr size_t kDefaultIterations = 10'000'0;
    static constexpr size_t kDefaultRepeats = 10;

    struct Config {
        platform::MeasurementEnvironmentOptions environment;
        size_t instr_cnt = kDefaultInstrCnt;
        size_t iterations = kDefaultIterations;
        size_t repeats = kDefaultRepeats;
    };

    ExecPortsMeasurer() : ExecPortsMeasurer(Config{}) {}
    explicit ExecPortsMeasurer(Config config)
        : config_(std::move(config)) {
        SPDLOG_INFO("[{}] configured: instr_cnt={}, iterations={}, repeats={}",
                    name(), config_.instr_cnt, config_.iterations, config_.repeats);
    }

    std::string_view name() const noexcept override { return "execution ports"; }

    void measure(core::CpuInfoData& data) override {
        SPDLOG_INFO("[{}] starting execution ports contention measurement", name());

        platform::ScopedMeasurementEnvironment environment{config_.environment};

        // Discover port events
        auto port_events = platform::discover_port_events();
        bool has_ports = false;
        if (port_events.empty()) {
            SPDLOG_WARN("[{}] No port events found. Check libpfm4, CPU vendor, and kernel support. "
                        "Falling back to time-only measurement.", name());
        } else {
            SPDLOG_INFO("[{}] Found {} port events", name(), port_events.size());
            has_ports = true;
        }

        std::unique_ptr<platform::pmc::PmcGroup> pmc;
        if (has_ports) {
            pmc = platform::pmc::PmcGroup::create_raw(port_events);
            if (!pmc) {
                SPDLOG_WARN("[{}] failed to open port counters, falling back to time-only", name());
                has_ports = false;
            }
        }

        // Generate test functions
        using InstrType = platform::arch::InstrType;
        std::vector<InstrType> add_only = {InstrType::ADD_IMM1};
        std::vector<InstrType> mul_only = {InstrType::LOAD_FROM_RCX};
        std::vector<InstrType> mixed = {InstrType::ADD_REG, InstrType::LOAD_FROM_RCX};

        void* add_func = generate_exec_ports_codegenerate(config_.instr_cnt, add_only);
        void* mul_func = generate_exec_ports_codegenerate(config_.instr_cnt, mul_only);
        void* mix_func = generate_exec_ports_codegenerate(config_.instr_cnt, mixed);

        if (!add_func || !mul_func || !mix_func) {
            SPDLOG_ERROR("[{}] failed to generate test functions", name());
            platform::arch::release_exec_ports_code();
            return;
        }

        // Helper to run one test and collect averaged results
        auto run_test = [&](void* func, const std::string& test_name) -> ExecPortsResult {
            auto f = reinterpret_cast<void(*)()>(func);

            // Warm-up (once)
            for (size_t i = 0; i < 100; ++i) f();

            std::vector<uint64_t> ticks_samples;
            std::vector<std::vector<uint64_t>> all_counts;

            for (size_t r = 0; r < config_.repeats; ++r) {
                if (pmc) {
                    pmc->reset();
                    pmc->enable();
                }

                uint64_t start_ticks = platform::arch::tick();

                for (size_t i = 0; i < config_.iterations; ++i) f();

                uint64_t end_ticks = platform::arch::tick();

                if (pmc) pmc->disable();

                uint64_t ticks = end_ticks - start_ticks;
                ticks_samples.push_back(ticks);

                if (pmc) {
                    auto cv = pmc->read();
                    all_counts.push_back(std::move(cv.values));
                }
            }

            // Average ticks
            double avg_ticks = std::accumulate(ticks_samples.begin(), ticks_samples.end(), 0.0) / config_.repeats;
            double ticks_std = 0.0;
            for (uint64_t t : ticks_samples) {
                double diff = static_cast<double>(t) - avg_ticks;
                ticks_std += diff * diff;
            }
            ticks_std = std::sqrt(ticks_std / config_.repeats);

            // Average port counts
            std::vector<uint64_t> avg_counts;
            if (!all_counts.empty()) {
                avg_counts.resize(all_counts[0].size(), 0);
                for (const auto& counts : all_counts) {
                    for (size_t i = 0; i < counts.size(); ++i) {
                        avg_counts[i] += counts[i];
                    }
                }
                for (size_t i = 0; i < avg_counts.size(); ++i) {
                    avg_counts[i] /= config_.repeats;
                }
            }

            SPDLOG_INFO("[{}] {}: avg_ticks = {:.1f} (std={:.1f})", name(), test_name, avg_ticks, ticks_std);
            if (!avg_counts.empty()) {
                for (size_t i = 0; i < port_events.size(); ++i) {
                    SPDLOG_INFO("  {} avg = {}", port_events[i], avg_counts[i]);
                }
            }

            return {test_name, avg_ticks, ticks_std, std::move(avg_counts)};
        };

        std::vector<ExecPortsResult> results;
        results.push_back(run_test(add_func, "ADD only"));
        results.push_back(run_test(mul_func, "MUL only"));
        results.push_back(run_test(mix_func, "ADD+MUL interleaved"));

        platform::arch::release_exec_ports_code();

        // Analyze contention
        PortContentionDecision decision = detectPortContention(results, port_events);
        data.execution_ports_independent = decision.different_ports;

        SPDLOG_INFO("[{}] decision: ADD and MUL use {} ports (confidence {:.2f}) - {}",
                    name(), decision.different_ports ? "different" : "the same",
                    decision.confidence, decision.reasoning);

        SPDLOG_INFO("[{}] measurement complete", name());
    }

private:
    PortContentionDecision detectPortContention(const std::vector<ExecPortsResult>& results,
                                                 const std::vector<std::string>& port_events) const {
        if (results.size() < 3) {
            return {false, 0.0, "insufficient data"};
        }

        const auto& add = results[0];
        const auto& mul = results[1];
        const auto& mix = results[2];

        bool mul_is_slower = (mul.avg_ticks > add.avg_ticks);
        double slow_ticks = mul_is_slower ? mul.avg_ticks : add.avg_ticks;
        double mix_ticks = mix.avg_ticks;

        const double improvement_threshold = 0.80;  // 20% faster
        bool ticks_indicate_diff = (mix_ticks < slow_ticks * improvement_threshold);

        bool ports_indicate_diff = false;
        double confidence = 0.5;
        std::string reasoning;

        if (!port_events.empty() && !add.avg_port_counts.empty() && !mul.avg_port_counts.empty() && !mix.avg_port_counts.empty()) {
            auto max_port_index = [](const std::vector<uint64_t>& counts) -> size_t {
                if (counts.empty()) return 0;
                return std::max_element(counts.begin(), counts.end()) - counts.begin();
            };
            size_t add_dominant = max_port_index(add.avg_port_counts);
            size_t mul_dominant = max_port_index(mul.avg_port_counts);

            if (add_dominant == mul_dominant) {
                ports_indicate_diff = false;
                reasoning = "dominant port same (" + port_events[add_dominant] + ")";
                confidence = 0.9;
            } else {
                uint64_t add_port_mix = mix.avg_port_counts[add_dominant];
                uint64_t mul_port_mix = mix.avg_port_counts[mul_dominant];
                uint64_t add_port_add = add.avg_port_counts[add_dominant];
                uint64_t mul_port_mul = mul.avg_port_counts[mul_dominant];

                bool add_port_active = (add_port_mix > add_port_add / 4);
                bool mul_port_active = (mul_port_mix > mul_port_mul / 4);
                if (add_port_active && mul_port_active) {
                    ports_indicate_diff = true;
                    reasoning = "ports " + port_events[add_dominant] + " and " + port_events[mul_dominant] +
                                " both active in mixed test";
                    confidence = 0.95;
                } else {
                    ports_indicate_diff = false;
                    reasoning = "mixed test does not fully utilize both dominant ports";
                    confidence = 0.7;
                }
            }
        }

        bool final_different;
        if (!port_events.empty()) {
            final_different = ports_indicate_diff;
            if (ticks_indicate_diff != ports_indicate_diff) {
                SPDLOG_WARN("[{}] ticks and port results disagree: ticks={}, ports={}",
                            name(), ticks_indicate_diff, ports_indicate_diff);
                reasoning += " (ticks disagreed)";
            }
        } else {
            final_different = ticks_indicate_diff;
            reasoning = ticks_indicate_diff ? "ticks-based (mix faster than slow)" : "ticks-based (mix not faster)";
            confidence = 0.6;
        }

        return {final_different, confidence, reasoning};
    }

    Config config_;
};

} // namespace silicon_probe::exec_ports
