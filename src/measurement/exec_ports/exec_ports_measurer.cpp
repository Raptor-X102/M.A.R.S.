// measurement/exec_ports/exec_ports_measurer.cpp
#include "measurement/exec_ports/exec_ports_measurer.hpp"
#include "measurement/common/statistics.hpp"

#include <algorithm>
#include <numeric>
#include <set>

namespace silicon_probe::exec_ports {

namespace statistics = silicon_probe::common::statistics;

ExecPortsMeasurer::ExecPortsMeasurer() : ExecPortsMeasurer(Config{}) {}

ExecPortsMeasurer::ExecPortsMeasurer(Config config) : config_(std::move(config)) {
    validateConfig();
    SPDLOG_DEBUG(
        "[{}] configured: instr_cnt={}, iterations={}, repeats={}, instr1 = [{}, {}], instr2 = [{}, {}]",
        name(),
        config_.instr_cnt,
        config_.iterations,
        config_.repeats,
        static_cast<int>(config_.instr1.instr_type),
        config_.instr1.instr_name,
        static_cast<int>(config_.instr2.instr_type),
        config_.instr2.instr_name
    );
}

std::string_view ExecPortsMeasurer::name() const noexcept { return "execution ports"; }

void ExecPortsMeasurer::validateConfig() {
    if (config_.instr_cnt == 0) {
        SPDLOG_WARN("[{}] instr_cnt is 0, resetting to default {}", name(), kDefaultInstrCnt);
        config_.instr_cnt = kDefaultInstrCnt;
    }
    if (config_.iterations == 0) {
        SPDLOG_WARN("[{}] iterations is 0, resetting to default {}", name(), kDefaultIterations);
        config_.iterations = kDefaultIterations;
    }
    if (config_.repeats == 0) {
        SPDLOG_WARN("[{}] repeats is 0, resetting to default {}", name(), kDefaultRepeats);
        config_.repeats = kDefaultRepeats;
    }
    if (config_.warmup_iterations == 0) {
        SPDLOG_DEBUG("[{}] warmup_iterations is 0, no warm-up will be performed", name());
    }
    if (config_.instr1.instr_name.empty()) {
        SPDLOG_WARN("[{}] instr1 name is empty, resetting to 'add reg'", name());
        config_.instr1.instr_name = "add reg";
        config_.instr1.instr_type = InstrType::ADD_REG;
    }
    if (config_.instr2.instr_name.empty()) {
        SPDLOG_WARN("[{}] instr2 name is empty, resetting to 'mul float'", name());
        config_.instr2.instr_name = "mul float";
        config_.instr2.instr_type = InstrType::MUL_FLOAT;
    }
    if (config_.instr1.instr_type == config_.instr2.instr_type &&
        config_.instr1.instr_name == config_.instr2.instr_name) {
        SPDLOG_WARN("[{}] instr1 and instr2 are identical, measurement will show full dependence", name());
    }
    auto clamp01 = [&](double& val, double default_val, const char* val_name) {
        if (val < 0.0 || val > 1.0) {
            SPDLOG_WARN("[{}] {} is {:.3f}, resetting to default {:.3f}", name(), val_name, val, default_val);
            val = default_val;
        }
    };
    clamp01(config_.strong_independence_time, kDefaultStrongIndependenceTime, "strong_independence_time");
    clamp01(config_.strong_dependence_time, kDefaultStrongDependenceTime, "strong_dependence_time");
    clamp01(config_.weak_independence_time, kDefaultWeakIndependenceTime, "weak_independence_time");
    clamp01(config_.weak_dependence_time, kDefaultWeakDependenceTime, "weak_dependence_time");
    clamp01(config_.time_weight, kDefaultTimeWeight, "time_weight");
    clamp01(config_.pmc_weight, kDefaultPmcWeight, "pmc_weight");
    clamp01(config_.k_strong_independence, kDefaultKStrongIndependence, "k_strong_independence");
    clamp01(config_.k_strong_dependence, kDefaultKStrongDependence, "k_strong_dependence");
    clamp01(config_.overlap_disagreement_high, kDefaultOverlapDisagreementHigh, "overlap_disagreement_high");
    clamp01(config_.overlap_disagreement_low, kDefaultOverlapDisagreementLow, "overlap_disagreement_low");
    clamp01(config_.active_port_threshold_ratio, kDefaultActivePortThresholdRatio, "active_port_threshold_ratio");
}

void ExecPortsMeasurer::measure(shared_types::CpuInfoData& data) {
    SPDLOG_INFO("[{}] starting execution ports contention measurement", name());

    // Release any previously generated code from this generator
    platform::arch::release_exec_ports_code();

    platform::ScopedMeasurementEnvironment environment{config_.environment};

    // Discover port events
    auto port_events = platform::discover_port_events(data);
    bool has_ports   = false;
    size_t num_events = port_events.size();
    if (port_events.empty()) {
        SPDLOG_WARN(
            "[{}] No port events found. Check libpfm4, CPU vendor, and kernel support. "
            "Falling back to time-only measurement.",
            name()
        );
    } else {
        SPDLOG_DEBUG("[{}] Found {} port events", name(), num_events);
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
    std::vector<InstrType> mixed       = {config_.instr1.instr_type, config_.instr2.instr_type};

    void* instr1_func = platform::arch::generate_exec_ports_code(config_.instr_cnt, instr1_only);
    void* instr2_func = platform::arch::generate_exec_ports_code(config_.instr_cnt, instr2_only);
    void* mix_func    = platform::arch::generate_exec_ports_code(config_.instr_cnt, mixed);

    if (!instr1_func || !instr2_func || !mix_func) {
        SPDLOG_ERROR("[{}] failed to generate test functions", name());
        platform::arch::release_exec_ports_code();
        return;
    }

    // Helper to run one test and collect averaged results
    auto run_test = [&](void* func, const std::string& test_name) -> ExecPortsResult {
        auto f = reinterpret_cast<void (*)()>(func);

        // Warm-up
        for (size_t i = 0; i < config_.warmup_iterations; ++i)
            f();

        std::vector<double> ticks_samples;
        std::vector<std::vector<uint64_t>> all_counts;

        for (size_t r = 0; r < config_.repeats; ++r) {
            if (pmc) {
                pmc->reset();
                pmc->enable();
            }

            uint64_t start_ticks = platform::arch::tick();

            for (size_t i = 0; i < config_.iterations; ++i)
                f();

            uint64_t end_ticks = platform::arch::tick();

            if (pmc)
                pmc->disable();

            double ticks = static_cast<double>(end_ticks - start_ticks);
            ticks_samples.push_back(ticks);

            if (pmc) {
                auto cv = pmc->read();
                all_counts.push_back(std::move(cv.values));
            }
        }

        // Average ticks
        auto stats = statistics::compute_stats(ticks_samples);
        double avg_ticks = stats.mean;
        double ticks_std = stats.stddev;

        // Average port counts
        std::vector<double> avg_counts;
        if (!all_counts.empty()) {
            avg_counts.resize(all_counts[0].size(), 0.0);
            for (const auto& counts : all_counts) {
                for (size_t i = 0; i < counts.size(); ++i) {
                    avg_counts[i] += static_cast<double>(counts[i]);
                }
            }
            for (double& v : avg_counts) v /= config_.repeats;
        }

        SPDLOG_DEBUG("[{}] {}: avg_ticks = {:.4g} (std={:.4g})", name(), test_name, avg_ticks, ticks_std);
        if (!avg_counts.empty()) {
            for (size_t i = 0; i < num_events; ++i) {
                SPDLOG_DEBUG("  {} avg = {:.4g}", port_events[i], static_cast<double>(avg_counts[i]));
            }
        }

        return {test_name, avg_ticks, ticks_std, std::move(avg_counts)};
    };

    std::vector<ExecPortsResult> results;
    results.push_back(run_test(instr1_func, config_.instr1.instr_name));
    results.push_back(run_test(instr2_func, config_.instr2.instr_name));
    results.push_back(
        run_test(mix_func, config_.instr1.instr_name + " + " + config_.instr2.instr_name + " interleaved")
    );

    // Release all generated functions after measurements
    platform::arch::release_exec_ports_code();

    // Analyze contention
    PortContentionDecision decision  = detectPortContention(results, port_events);
    data.execution_ports_independent = decision.different_ports;

    SPDLOG_INFO(
        "[{}] decision: instruction1 ({}) and instruction2 ({}) use {} ports (confidence {:.2f}) - {}",
        name(),
        config_.instr1.instr_name,
        config_.instr2.instr_name,
        decision.different_ports ? "different" : "the same",
        decision.confidence,
        decision.reasoning
    );

    SPDLOG_INFO("[{}] measurement complete", name());
}

PortContentionDecision ExecPortsMeasurer::detectPortContention(
    const std::vector<ExecPortsResult>& results,
    const std::vector<std::string>& port_events
) {
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
    double k       = 0.5;
    if (t_dep > t_indep) {
        k = (tm - t_indep) / (t_dep - t_indep);
        k = std::clamp(k, 0.0, 1.0);
    }
    double time_conf_indep = 1.0 - k;
    double time_conf_dep   = k;

    std::string time_summary;
    if (k <= config_.strong_independence_time) {
        time_summary = "STRONG INDEPENDENCE";
    } else if (k >= config_.strong_dependence_time) {
        time_summary = "STRONG DEPENDENCE";
    } else if (k <= config_.weak_independence_time) {
        time_summary = "WEAK INDEPENDENCE";
    } else if (k >= config_.weak_dependence_time) {
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

    if (!port_events.empty() && !r1.avg_port_counts.empty() && !r2.avg_port_counts.empty()) {
        auto active_ports = [&](const std::vector<double>& counts) -> std::vector<size_t> {
            if (counts.empty()) return {};
            double max_val = *std::max_element(counts.begin(), counts.end());
            if (max_val == 0.0) return {};
            std::vector<size_t> ports;
            for (size_t i = 0; i < counts.size(); ++i) {
                if (counts[i] >= max_val * config_.active_port_threshold_ratio)
                    ports.push_back(i);
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
                    size_t last_dot  = name.rfind('.');
                    if (last_dot != std::string::npos)
                        name = name.substr(last_dot + 1);
                    if (!s.empty())
                        s += ",";
                    s += name;
                }
                return s;
            };

            ports1_str = port_names(ports1);
            ports2_str = port_names(ports2);

            std::vector<size_t> inter;
            std::set_intersection(
                ports1.begin(),
                ports1.end(),
                ports2.begin(),
                ports2.end(),
                std::back_inserter(inter)
            );
            inter_str       = port_names(inter);
            size_t inter_sz = inter.size();
            size_t union_sz = ports1.size() + ports2.size() - inter_sz;
            overlap         = (union_sz == 0) ? 0.5 : static_cast<double>(inter_sz) / union_sz;
            pmc_conf_indep  = 1.0 - overlap;
            pmc_conf_dep    = overlap;

            if (overlap <= config_.strong_independence_overlap + 1e-12) {
                pmc_summary = "STRONG INDEPENDENCE (no shared ports)";
            } else if (overlap >= config_.strong_dependence_overlap) {
                pmc_summary = "STRONG DEPENDENCE (most ports shared)";
            } else if (overlap <= config_.weak_independence_overlap) {
                pmc_summary = "WEAK INDEPENDENCE (few shared ports)";
            } else if (overlap >= config_.weak_dependence_overlap) {
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
    bool final_diff;
    double final_conf;
    std::string reasoning;

    if (k <= config_.k_strong_independence) {
        final_diff = true;
        final_conf = 1.0 - k;
        reasoning  = "TIME: " + time_summary + " (k=" + std::to_string(k) + ") - independent";
        if (overlap > config_.overlap_disagreement_high) {
            reasoning += " [PMC disagrees but time is definitive]";
        }
    } else if (k >= config_.k_strong_dependence) {
        final_diff = false;
        final_conf = k;
        reasoning  = "TIME: " + time_summary + " (k=" + std::to_string(k) + ") - dependent";
        if (overlap < config_.overlap_disagreement_low) {
            reasoning += " [PMC disagrees but time is definitive]";
        }
    } else {
        // Normalize weights to sum to 1
        double total_weight = config_.time_weight + config_.pmc_weight;
        double norm_time, norm_pmc;
        if (total_weight > 0.0) {
            norm_time = config_.time_weight / total_weight;
            norm_pmc  = config_.pmc_weight / total_weight;
        } else {
            // Fallback: equal weights if both are zero
            norm_time = 0.5;
            norm_pmc  = 0.5;
        }
        
        double comb_indep = time_conf_indep * norm_time + pmc_conf_indep * norm_pmc;
        double comb_dep   = time_conf_dep * norm_time + pmc_conf_dep * norm_pmc;
        
        final_diff = (comb_indep > comb_dep);
        final_conf = std::max(comb_indep, comb_dep);
        reasoning  = "Combined (time " + std::to_string(config_.time_weight) + "%, PMC " + 
                     std::to_string(config_.pmc_weight) + "%, normalized to " +
                     std::to_string(norm_time) + "/" + std::to_string(norm_pmc) + 
                     "): independent=" + std::to_string(comb_indep) +
                     ", dependent=" + std::to_string(comb_dep) + " -> " +
                     std::string(final_diff ? "independent" : "dependent");
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
    detailed +=
        "  Result: ports are " + std::string(final_diff ? "DIFFERENT (independent)" : "THE SAME (dependent)") + "\n";
    detailed += "================================================\n";

    SPDLOG_DEBUG(detailed);

    return {final_diff, final_conf, reasoning};
}

}  // namespace silicon_probe::exec_ports
