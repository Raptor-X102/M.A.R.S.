#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

#define private public
#include "measurement/branch_history_table/branch_history_table_measurer.hpp"
#undef private

#include <gtest/gtest.h>

namespace silicon_probe::branch_history_table {
namespace {

struct BhtCase {
    const char* name;
    std::vector<BranchHistoryTableMeasurer::BranchHistoryTableResult> results;
    int expected_size;
};

TEST(BranchHistoryTableTableTest, DetectsSaturationFromTable) {
    const std::vector<BhtCase> cases{
        {
            "threshold_040",
            {
                {16, 0.05},
                {32, 0.07},
                {64, 0.10},
                {128, 0.20},
                {256, 0.41},
                {512, 0.44},
            },
            256,
        },
        {
            "threshold_035_fallback",
            {
                {16, 0.04},
                {32, 0.05},
                {64, 0.08},
                {128, 0.18},
                {256, 0.36},
                {512, 0.39},
            },
            256,
        },
        {
            "inflection_fallback",
            {
                {16, 0.05},
                {32, 0.06},
                {64, 0.07},
                {128, 0.20},
                {256, 0.21},
            },
            64,
        },
        {
            "insufficient_points",
            {
                {16, 0.05},
                {32, 0.06},
                {64, 0.08},
            },
            -1,
        },
    };

    for (const auto& test_case : cases) {
        SCOPED_TRACE(test_case.name);
        BranchHistoryTableMeasurer measurer;
        EXPECT_EQ(measurer.detectBHTSaturation(test_case.results), test_case.expected_size);
    }
}

}  // namespace
}  // namespace silicon_probe::branch_history_table
