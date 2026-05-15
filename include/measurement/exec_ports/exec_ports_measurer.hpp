// measurement/exec_ports/exec_ports_measurer.hpp
#pragma once

#include <algorithm>
#include <cmath>
#include <numeric>
#include <string>
#include <vector>

#include "core/measurer.hpp"
#include "infra/logging.hpp"
#include "platform/arch.hpp"
#include "platform/events_discovery.hpp"
#include "platform/os.hpp"
#include "platform/pmc.hpp"

namespace silicon_probe::exec_ports {

struct ExecPortsResult {
    std::string test_name;
    double avg_ticks;  // average ticks per repeat
    double ticks_std;  // standard deviation of ticks
    std::vector<double> avg_port_counts;
};

struct PortContentionDecision {
    bool different_ports;
    double confidence;
    std::string reasoning;
};

struct InstructionData {
    platform::arch::InstrType instr_type;
    std::string instr_name;
};

class ExecPortsMeasurer final : public core::Measurer {
   public:
    using InstrType = platform::arch::InstrType;

    static constexpr size_t kDefaultInstrCnt   = 100000;
    static constexpr size_t kDefaultIterations = 10'000;
    static constexpr size_t kDefaultRepeats    = 10;

    // counters
        static constexpr double kDefaultStrongIndependenceOverlap = 0.0;
    static constexpr double kDefaultStrongDependenceOverlap = 0.8;
    static constexpr double kDefaultWeakIndependenceOverlap = 0.3;
    static constexpr double kDefaultWeakDependenceOverlap = 0.5;
    
    // latency (time analysis)
    static constexpr double kDefaultStrongIndependenceTime = 0.1;
    static constexpr double kDefaultStrongDependenceTime = 0.9;
    static constexpr double kDefaultWeakIndependenceTime = 0.4;
    static constexpr double kDefaultWeakDependenceTime = 0.6;
    
    // combined decision weights and thresholds
    static constexpr double kDefaultTimeWeight = 0.6;
    static constexpr double kDefaultPmcWeight = 0.4;
    static constexpr double kDefaultKStrongIndependence = 0.1;   // for combined decision
    static constexpr double kDefaultKStrongDependence = 0.9;
    static constexpr double kDefaultOverlapDisagreementHigh = 0.5;
    static constexpr double kDefaultOverlapDisagreementLow = 0.3;
    
    // active ports detection
    static constexpr double kDefaultActivePortThresholdRatio = 0.1; // 10% of max

    struct Config {
        bool enabled = true;
        platform::MeasurementEnvironmentOptions environment;
        size_t instr_cnt         = kDefaultInstrCnt;
        size_t iterations        = kDefaultIterations;
        size_t repeats           = kDefaultRepeats;
        size_t warmup_iterations = 100;
        InstructionData instr1   = {InstrType::ADD_REG, "add reg"};
        InstructionData instr2   = {InstrType::MUL_FLOAT, "mul float"};
        
        // Overlap thresholds
        double strong_independence_overlap = kDefaultStrongIndependenceOverlap;
        double strong_dependence_overlap = kDefaultStrongDependenceOverlap;
        double weak_independence_overlap = kDefaultWeakIndependenceOverlap;
        double weak_dependence_overlap   = kDefaultWeakDependenceOverlap;
        
        // Time analysis thresholds
        double strong_independence_time = kDefaultStrongIndependenceTime;
        double strong_dependence_time = kDefaultStrongDependenceTime;
        double weak_independence_time = kDefaultWeakIndependenceTime;
        double weak_dependence_time   = kDefaultWeakDependenceTime;
        
        // Combined decision
        double time_weight = kDefaultTimeWeight;
        double pmc_weight  = kDefaultPmcWeight;
        double k_strong_independence = kDefaultKStrongIndependence;
        double k_strong_dependence   = kDefaultKStrongDependence;
        double overlap_disagreement_high = kDefaultOverlapDisagreementHigh;
        double overlap_disagreement_low  = kDefaultOverlapDisagreementLow;
        
        // Active ports detection
        double active_port_threshold_ratio = kDefaultActivePortThresholdRatio;
    };

    ExecPortsMeasurer();
    explicit ExecPortsMeasurer(Config config);

    std::string_view name() const noexcept override;
    void measure(shared_types::CpuInfoData& data) override;

   private:
    Config config_;

    void validateConfig();
    PortContentionDecision
    detectPortContention(const std::vector<ExecPortsResult>& results, const std::vector<std::string>& port_events);
};

}  // namespace silicon_probe::exec_ports
