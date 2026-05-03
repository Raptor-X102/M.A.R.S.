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
    std::vector<uint64_t> avg_port_counts;
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

    static constexpr size_t kDefaultInstrCnt = 100000;
    static constexpr size_t kDefaultIterations = 10'000;
    static constexpr size_t kDefaultRepeats = 10;

    struct Config {
        bool enabled = true;
        platform::MeasurementEnvironmentOptions environment;
        size_t instr_cnt = kDefaultInstrCnt;
        size_t iterations = kDefaultIterations;
        size_t repeats = kDefaultRepeats;
        size_t warmup_iterations = 100;
        InstructionData instr1 = {InstrType::ADD_REG, "add reg"};
        InstructionData instr2 = {InstrType::MUL_FLOAT, "mul float"};
    };

    ExecPortsMeasurer();
    explicit ExecPortsMeasurer(Config config);

    std::string_view name() const noexcept override;
    void measure(shared_types::CpuInfoData& data) override;

private:
    Config config_;

    static PortContentionDecision detectPortContention(const std::vector<ExecPortsResult>& results,
                                                       const std::vector<std::string>& port_events);
};

}  // namespace silicon_probe::exec_ports
