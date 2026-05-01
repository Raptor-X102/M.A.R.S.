#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <string>
#include <vector>

#define private public
#include "measurement/exec_ports/exec_ports_measurer.hpp"
#undef private

#include <gtest/gtest.h>

namespace silicon_probe::exec_ports {
namespace {

struct ExecPortsCase {
    const char* name;
    std::vector<ExecPortsResult> results;
    std::vector<std::string> port_events;
    bool expected_different_ports;
    double expected_confidence;
};

TEST(ExecPortsTableTest, DetectsPortContentionFromTable) {
    const std::vector<ExecPortsCase> cases{
        {
            "strong_time_independence",
            {
                {"instr1", 100.0, 0.1, {}},
                {"instr2", 100.0, 0.1, {}},
                {"mixed", 50.0, 0.1, {}},
            },
            {},
            true,
            1.0,
        },
        {
            "strong_time_dependence",
            {
                {"instr1", 100.0, 0.1, {}},
                {"instr2", 100.0, 0.1, {}},
                {"mixed", 100.0, 0.1, {}},
            },
            {},
            false,
            1.0,
        },
        {
            "ambiguous_time_but_independent_ports",
            {
                {"instr1", 100.0, 0.1, {100, 0}},
                {"instr2", 100.0, 0.1, {0, 100}},
                {"mixed", 75.0, 0.1, {50, 50}},
            },
            {"cpu.port_0", "cpu.port_1"},
            true,
            0.7,
        },
        {
            "ambiguous_time_but_shared_ports",
            {
                {"instr1", 100.0, 0.1, {100, 0}},
                {"instr2", 100.0, 0.1, {100, 0}},
                {"mixed", 75.0, 0.1, {100, 0}},
            },
            {"cpu.port_0", "cpu.port_1"},
            false,
            0.7,
        },
    };

    for (const auto& test_case : cases) {
        SCOPED_TRACE(test_case.name);
        ExecPortsMeasurer measurer;
        const auto decision = measurer.detectPortContention(test_case.results, test_case.port_events);
        EXPECT_EQ(decision.different_ports, test_case.expected_different_ports);
        EXPECT_NEAR(decision.confidence, test_case.expected_confidence, 1e-6);
    }
}

}  // namespace
}  // namespace silicon_probe::exec_ports
