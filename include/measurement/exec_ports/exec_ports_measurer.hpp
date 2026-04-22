// ExecPortsMeasurer.hpp
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

struct IstructionData {
    platform::arch::InstrType instr_type;
    std::string instr_name;
};

class ExecPortsMeasurer final : public core::Measurer {
public:
    using InstrType = platform::arch::InstrType;

    static constexpr size_t kDefaultInstrCnt = 100000;
    static constexpr size_t kDefaultIterations = 10'000;
    static constexpr size_t kDefaultRepeats = 10;

    struct Config {
        platform::MeasurementEnvironmentOptions environment;
        size_t instr_cnt = kDefaultInstrCnt;
        size_t iterations = kDefaultIterations;
        size_t repeats = kDefaultRepeats;
        IstructionData instr1 = { InstrType::ADD_REG, "add reg" };
        IstructionData instr2 = { InstrType::MUL_FLOAT, "mul float" };
    };

    ExecPortsMeasurer() : ExecPortsMeasurer(Config{}) {}
    explicit ExecPortsMeasurer(Config config)
        : config_(std::move(config)) {
        SPDLOG_INFO("[{}] configured: instr_cnt={}, iterations={}, repeats={}, instr1 = [{}, {}], instr2 = [{}, {}]",
                    name(), config_.instr_cnt, config_.iterations, config_.repeats, 
                    static_cast<int>(config_.instr1.instr_type), config_.instr1.instr_name, 
                    static_cast<int>(config_.instr2.instr_type), config_.instr2.instr_name);
    }

    std::string_view name() const noexcept override { return "execution ports"; }

    void measure(core::CpuInfoData& data) override {
        SPDLOG_INFO("[{}] starting execution ports contention measurement", name());

        // Release any previously generated code from this generator
        platform::arch::release_exec_ports_code();

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
        std::vector<InstrType> instr1_only = {config_.instr1.instr_type};
        std::vector<InstrType> instr2_only = {config_.instr2.instr_type};
        std::vector<InstrType> mixed = {config_.instr1.instr_type, config_.instr2.instr_type};

        void* instr1_func = generate_exec_ports_codegenerate(config_.instr_cnt, instr1_only);
        void* instr2_func  = generate_exec_ports_codegenerate(config_.instr_cnt, instr2_only);
        void* mix_func = generate_exec_ports_codegenerate(config_.instr_cnt, mixed);

        if (!instr1_func || !instr2_func || !mix_func) {
            SPDLOG_ERROR("[{}] failed to generate test functions", name());
            platform::arch::release_exec_ports_code();
            return;
        }

        // Helper to run one test and collect averaged results
        auto run_test = [&](void* func, const std::string& test_name) -> ExecPortsResult {
            auto f = reinterpret_cast<void(*)()>(func);

            // Warm-up
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
                //SPDLOG_INFO("[repeat {}]: ticks = {}", r, ticks);
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

            SPDLOG_INFO("[{}] {}: avg_ticks = {:.4g} (std={:.4g})", name(), test_name, avg_ticks, ticks_std);
            if (!avg_counts.empty()) {
                for (size_t i = 0; i < port_events.size(); ++i) {
                    SPDLOG_INFO("  {} avg = {:.4g}", port_events[i], static_cast<double>(avg_counts[i]));
                }
            }

            return {test_name, avg_ticks, ticks_std, std::move(avg_counts)};
        };

        std::vector<ExecPortsResult> results;
        results.push_back(run_test(instr1_func, config_.instr1.instr_name));
        results.push_back(run_test(instr2_func, config_.instr2.instr_name));
        results.push_back(run_test(mix_func, config_.instr1.instr_name + 
                                             " + "                     + 
                                             config_.instr2.instr_name + 
                                             " interleaved"));

        // Release all generated functions after measurements
        platform::arch::release_exec_ports_code();

        // Analyze contention
        PortContentionDecision decision = detectPortContention(results, port_events);
        data.execution_ports_independent = decision.different_ports;

        SPDLOG_INFO("[{}] decision: instruction1 ({}) and instruction2 ({}) use {} ports (confidence {:.2f}) - {}",
                    name(), config_.instr1.instr_name, config_.instr2.instr_name, decision.different_ports ? "different" : "the same",
                    decision.confidence, decision.reasoning);

        SPDLOG_INFO("[{}] measurement complete", name());
    }

private:
    PortContentionDecision detectPortContention(const std::vector<ExecPortsResult>& results,
                                                const std::vector<std::string>& port_events) const {
        if (results.size() < 3) {
            return {false, 0.0, "insufficient data"};
        }

        const auto& r1 = results[0];
        const auto& r2 = results[1];
        const auto& rm = results[2];

        double t1 = r1.avg_ticks;
        double t2 = r2.avg_ticks;
        double tm = rm.avg_ticks;

        // ===== TIME ANALYSIS =====
        double t_indep = 0.5 * std::max(t1, t2);
        double t_dep   = 0.5 * (t1 + t2);
        double k = 0.5;
        if (t_dep > t_indep) {
            k = (tm - t_indep) / (t_dep - t_indep);
            k = std::clamp(k, 0.0, 1.0);
        }
        double time_conf_indep = 1.0 - k;
        double time_conf_dep   = k;

        std::string time_summary;
        if (k <= 0.1) {
            time_summary = "STRONG INDEPENDENCE (k <= 0.1)";
        } else if (k >= 0.9) {
            time_summary = "STRONG DEPENDENCE (k >= 0.9)";
        } else if (k <= 0.4) {
            time_summary = "WEAK INDEPENDENCE";
        } else if (k >= 0.6) {
            time_summary = "WEAK DEPENDENCE";
        } else {
            time_summary = "AMBIGUOUS";
        }

        // ===== PMC ANALYSIS =====
        double pmc_conf_indep = 0.5;
        double pmc_conf_dep   = 0.5;
        std::string pmc_summary;
        std::string ports1_str, ports2_str, inter_str;
        double overlap = 0.5;

        if (!port_events.empty() && !r1.avg_port_counts.empty() &&
            !r2.avg_port_counts.empty()) {

            auto active_ports = [&](const std::vector<uint64_t>& counts) -> std::vector<size_t> {
                if (counts.empty()) return {};
                uint64_t max_val = *std::max_element(counts.begin(), counts.end());
                if (max_val == 0) return {};
                std::vector<size_t> ports;
                for (size_t i = 0; i < counts.size(); ++i) {
                    if (counts[i] >= max_val / 10) ports.push_back(i);
                }
                return ports;
            };

            auto ports1 = active_ports(r1.avg_port_counts);
            auto ports2 = active_ports(r2.avg_port_counts);

            if (!ports1.empty() && !ports2.empty()) {
                auto port_names = [&](const std::vector<size_t>& idxs) -> std::string {
                    std::string s;
                    for (size_t idx : idxs) {
                        std::string name = port_events[idx];
                        // Shorten names for readability
                        size_t last_dot = name.rfind('.');
                        if (last_dot != std::string::npos) name = name.substr(last_dot + 1);
                        if (!s.empty()) s += ",";
                        s += name;
                    }
                    return s;
                };

                ports1_str = port_names(ports1);
                ports2_str = port_names(ports2);

                std::vector<size_t> inter;
                std::set_intersection(ports1.begin(), ports1.end(),
                                      ports2.begin(), ports2.end(),
                                      std::back_inserter(inter));
                inter_str = port_names(inter);
                size_t inter_sz = inter.size();
                size_t union_sz = ports1.size() + ports2.size() - inter_sz;
                overlap = (union_sz == 0) ? 0.5 : static_cast<double>(inter_sz) / union_sz;
                pmc_conf_indep = 1.0 - overlap;
                pmc_conf_dep   = overlap;

                if (overlap == 0.0) {
                    pmc_summary = "STRONG INDEPENDENCE (no shared ports)";
                } else if (overlap >= 0.8) {
                    pmc_summary = "STRONG DEPENDENCE (most ports shared)";
                } else if (overlap <= 0.3) {
                    pmc_summary = "WEAK INDEPENDENCE (few shared ports)";
                } else if (overlap >= 0.5) {
                    pmc_summary = "WEAK DEPENDENCE (significant overlap)";
                } else {
                    pmc_summary = "AMBIGUOUS";
                }
            } else {
                pmc_summary = "No active ports detected";
            }
        } else {
            pmc_summary = "PMC data unavailable";
        }

        // ===== COMBINED DECISION =====
        // Trust time if it's confident (k <= 0.1 or k >= 0.9)
        // Otherwise use weighted average
        bool final_diff;
        double final_conf;
        std::string reasoning;

        if (k <= 0.1) {
            final_diff = true;
            final_conf = 1.0 - k;
            reasoning = "TIME: " + time_summary + " (k=" + std::to_string(k) + ") - independent";
            if (overlap > 0.5) {
                reasoning += " [PMC disagrees but time is definitive]";
            }
        } else if (k >= 0.9) {
            final_diff = false;
            final_conf = k;
            reasoning = "TIME: " + time_summary + " (k=" + std::to_string(k) + ") - dependent";
            if (overlap < 0.3) {
                reasoning += " [PMC disagrees but time is definitive]";
            }
        } else {
            // Weighted average (time 60%, PMC 40%)
            double comb_indep = time_conf_indep * 0.6 + pmc_conf_indep * 0.4;
            double comb_dep   = time_conf_dep * 0.6 + pmc_conf_dep * 0.4;
            final_diff = (comb_indep > comb_dep);
            final_conf = std::max(comb_indep, comb_dep);
            reasoning = "Combined (time 60%, PMC 40%): independent=" +
                        std::to_string(comb_indep) + ", dependent=" + std::to_string(comb_dep) +
                        " -> " + std::string(final_diff ? "independent" : "dependent");
        }

        // ===== BUILD DETAILED OUTPUT =====
        std::string detailed = "\n========== PORT CONTENTION ANALYSIS ==========\n";
        detailed += "\n[TIME ANALYSIS]\n";
        detailed += "  t1 (" + r1.test_name + "): " + std::to_string(t1) + " ticks\n";
        detailed += "  t2 (" + r2.test_name + "): " + std::to_string(t2) + " ticks\n";
        detailed += "  tm (mixed): " + std::to_string(tm) + " ticks\n";
        detailed += "  t_indep (0.5 * max): " + std::to_string(t_indep) + "\n";
        detailed += "  t_dep (0.5 * sum): " + std::to_string(t_dep) + "\n";
        detailed += "  k = (tm - t_indep)/(t_dep - t_indep) = " + std::to_string(k) + "\n";
        detailed += "  -> independent confidence: " + std::to_string(time_conf_indep) + "\n";
        detailed += "  -> dependent confidence: " + std::to_string(time_conf_dep) + "\n";
        detailed += "  -> verdict: " + time_summary + "\n";

        detailed += "\n[PMC ANALYSIS]\n";
        if (!ports1_str.empty()) {
            detailed += "  Active ports for '" + r1.test_name + "': {" + ports1_str + "}\n";
            detailed += "  Active ports for '" + r2.test_name + "': {" + ports2_str + "}\n";
            detailed += "  Shared ports: {" + inter_str + "}\n";
            detailed += "  Overlap ratio (intersection/union): " + std::to_string(overlap) + "\n";
            detailed += "  -> independent confidence: " + std::to_string(pmc_conf_indep) + "\n";
            detailed += "  -> dependent confidence: " + std::to_string(pmc_conf_dep) + "\n";
            detailed += "  -> verdict: " + pmc_summary + "\n";
        } else {
            detailed += "  " + pmc_summary + "\n";
        }

        detailed += "\n[FINAL DECISION]\n";
        detailed += "  " + reasoning + "\n";
        detailed += "  Confidence: " + std::to_string(final_conf) + "\n";
        detailed += "  Result: ports are " + std::string(final_diff ? "DIFFERENT (independent)" : "THE SAME (dependent)") + "\n";
        detailed += "================================================\n";

        SPDLOG_INFO(detailed);

        return {final_diff, final_conf, reasoning};
    }

    Config config_;
};

} // namespace silicon_probe::exec_ports
