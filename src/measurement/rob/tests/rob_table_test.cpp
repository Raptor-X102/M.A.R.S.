#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <numeric>
#include <random>
#include <vector>

#define private public
#include "measurement/rob/rob_measurer.hpp"
#undef private

#include <gtest/gtest.h>

namespace silicon_probe::rob {
namespace {

struct RobCase {
    const char* name;
    std::vector<RobMeasurer::Result> results;
    int expected_size;
};

TEST(RobTableTest, DetectsRobSaturationFromTable) {
    const std::vector<RobCase> cases{
        {
            "stable_threshold_crossing",
            {
                {50, 0.9, 1.00, 1.1},
                {60, 0.9, 1.02, 1.1},
                {70, 0.9, 0.98, 1.1},
                {80, 0.9, 1.01, 1.1},
                {90, 0.9, 1.00, 1.1},
                {100, 1.0, 1.20, 1.3},
                {110, 1.0, 1.22, 1.3},
                {120, 1.0, 1.25, 1.3},
                {130, 1.0, 1.28, 1.3},
                {140, 1.0, 1.30, 1.3},
            },
            101,
        },
        {
            "fallback_to_max_diff",
            {
                {50, 0.9, 1.00, 1.1},
                {60, 0.9, 1.01, 1.1},
                {70, 0.9, 0.99, 1.1},
                {80, 0.9, 1.00, 1.1},
                {90, 0.9, 1.00, 1.1},
                {100, 1.0, 1.14, 1.3},
                {110, 1.0, 1.80, 2.0},
                {120, 1.0, 1.10, 1.3},
                {130, 1.0, 1.12, 1.3},
                {140, 1.0, 1.08, 1.3},
            },
            111,
        },
        {
            "insufficient_points",
            {
                {50, 0.9, 1.00, 1.1},
                {60, 0.9, 1.01, 1.1},
                {70, 0.9, 0.99, 1.1},
                {80, 0.9, 1.00, 1.1},
                {90, 0.9, 1.00, 1.1},
            },
            -1,
        },
    };

    for (const auto& test_case : cases) {
        SCOPED_TRACE(test_case.name);
        RobMeasurer measurer;
        EXPECT_EQ(measurer.detectRobSaturation(test_case.results), test_case.expected_size);
    }
}

}  // namespace
}  // namespace silicon_probe::rob
