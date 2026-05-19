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

/** @brief Result for one generated code test. */
struct ExecPortsResult {
    std::string test_name;                ///< Test name.
    double avg_ticks;                     ///< Average time in TSC ticks.
    double ticks_std;                     ///< Std. dev. of the time.
    std::vector<double> avg_port_counts;  ///< Average value for each port event.
};

/** @brief Final decision about port sharing. */
struct PortContentionDecision {
    bool different_ports;   ///< `true` if the two tests look independent.
    double confidence;      ///< Confidence in the range [0, 1].
    std::string reasoning;  ///< Short reason for the result.
};

/** @brief Instruction description for generated code. */
struct InstructionData {
    platform::arch::InstrType instr_type;  ///< Instruction type for the JIT generator.
    std::string instr_name;                ///< Short name for logs and output.
};

/**
 * @brief Checks if two instruction streams use the same execution ports.
 *
 * It measures each stream alone and then together.
 * Then it uses time and counter data to make one final decision.
 */
class ExecPortsMeasurer final : public core::Measurer {
   public:
    using InstrType = platform::arch::InstrType;

    static constexpr size_t kDefaultInstrCnt   = 100000;
    static constexpr size_t kDefaultIterations = 10'000;
    static constexpr size_t kDefaultRepeats    = 10;

    // counters
    static constexpr double kDefaultStrongIndependenceOverlap = 0.0;
    static constexpr double kDefaultStrongDependenceOverlap   = 0.8;
    static constexpr double kDefaultWeakIndependenceOverlap   = 0.3;
    static constexpr double kDefaultWeakDependenceOverlap     = 0.5;

    // latency (time analysis)
    static constexpr double kDefaultStrongIndependenceTime = 0.1;
    static constexpr double kDefaultStrongDependenceTime   = 0.9;
    static constexpr double kDefaultWeakIndependenceTime   = 0.4;
    static constexpr double kDefaultWeakDependenceTime     = 0.6;

    // combined decision weights and thresholds
    static constexpr double kDefaultTimeWeight              = 0.6;
    static constexpr double kDefaultPmcWeight               = 0.4;
    static constexpr double kDefaultKStrongIndependence     = 0.1;  // for combined decision
    static constexpr double kDefaultKStrongDependence       = 0.9;
    static constexpr double kDefaultOverlapDisagreementHigh = 0.5;
    static constexpr double kDefaultOverlapDisagreementLow  = 0.3;

    // active ports detection
    static constexpr double kDefaultActivePortThresholdRatio = 0.1;  // 10% of max

    /** @brief Settings for the execution-port benchmark. */
    struct Config {
        bool enabled = true;                                             ///< Turn this measurer on or off.
        platform::MeasurementEnvironmentOptions environment;             ///< CPU and scheduler settings for the run.
        size_t instr_cnt         = kDefaultInstrCnt;                     ///< Number of instructions in generated code.
        size_t iterations        = kDefaultIterations;                   ///< Number of calls inside one repeat.
        size_t repeats           = kDefaultRepeats;                      ///< Number of repeats for one point.
        size_t warmup_iterations = 100;                                  ///< Warm-up calls before timing.
        InstructionData instr1   = {InstrType::ADD_REG, "add reg"};      ///< First instruction stream.
        InstructionData instr2   = {InstrType::MUL_FLOAT, "mul float"};  ///< Second instruction stream.

        double strong_independence_overlap =
            kDefaultStrongIndependenceOverlap;  ///< Counter overlap for strong independence.
        double strong_dependence_overlap = kDefaultStrongDependenceOverlap;  ///< Counter overlap for strong dependence.
        double weak_independence_overlap = kDefaultWeakIndependenceOverlap;  ///< Counter overlap for weak independence.
        double weak_dependence_overlap   = kDefaultWeakDependenceOverlap;    ///< Counter overlap for weak dependence.

        double strong_independence_time = kDefaultStrongIndependenceTime;  ///< Time score for strong independence.
        double strong_dependence_time   = kDefaultStrongDependenceTime;    ///< Time score for strong dependence.
        double weak_independence_time   = kDefaultWeakIndependenceTime;    ///< Time score for weak independence.
        double weak_dependence_time     = kDefaultWeakDependenceTime;      ///< Time score for weak dependence.

        double time_weight               = kDefaultTimeWeight;           ///< Weight of time data in the final score.
        double pmc_weight                = kDefaultPmcWeight;            ///< Weight of counter data in the final score.
        double k_strong_independence     = kDefaultKStrongIndependence;  ///< Final score for strong independence.
        double k_strong_dependence       = kDefaultKStrongDependence;    ///< Final score for strong dependence.
        double overlap_disagreement_high = kDefaultOverlapDisagreementHigh;  ///< High limit for disagreement checks.
        double overlap_disagreement_low  = kDefaultOverlapDisagreementLow;   ///< Low limit for disagreement checks.

        double active_port_threshold_ratio = kDefaultActivePortThresholdRatio;  ///< Limit for an active port event.
    };

    /** @brief Builds the measurer with default settings. */
    ExecPortsMeasurer();

    /**
     * @brief Builds the measurer with custom settings.
     * @param config Settings for the next runs.
     */
    explicit ExecPortsMeasurer(Config config);

    /**
     * @brief Returns the benchmark name.
     * @return Name used in logs and output.
     */
    std::string_view name() const noexcept override;

    /**
     * @brief Runs the execution-port benchmark.
     * @param data Output data. The function writes the final port result here.
     */
    void measure(shared_types::CpuInfoData& data) override;

   private:
    Config config_;

    /** @brief Fixes bad setting values before the run. */
    void validateConfig();

    /**
     * @brief Makes the final decision from time and counter data.
     * @param results Results for test 1, test 2, and the mixed test.
     * @param port_events Port event names in the same order as avg_port_counts.
     * @return Final result with confidence and reason.
     */
    PortContentionDecision detectPortContention(
        const std::vector<ExecPortsResult>& results, const std::vector<std::string>& port_events
    );
};

}  // namespace silicon_probe::exec_ports
