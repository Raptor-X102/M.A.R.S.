#include <algorithm>
#include <bitset>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

#define private public
#include "measurement/cache/cache_measurer.hpp"
#undef private

#include <gtest/gtest.h>

namespace silicon_probe::cache {
namespace {

struct GrowthFactorCase {
    const char* name;
    size_t size_bytes;
    double expected_growth_factor;
};

TEST(CacheMeasurerTableTest, UsesExpectedGrowthFactorsFromTable) {
    const CacheMeasurer measurer;
    const std::vector<GrowthFactorCase> cases{
        {"l1_range", 64 * 1024, CacheMeasurer::kL1GrowthFactor},
        {"l2_range", 512 * 1024, CacheMeasurer::kL2GrowthFactor},
        {"l3_range", 8 * 1024 * 1024, CacheMeasurer::kL3GrowthFactor},
    };

    for (const auto& test_case : cases) {
        SCOPED_TRACE(test_case.name);
        EXPECT_DOUBLE_EQ(measurer.growth_factor_for(test_case.size_bytes), test_case.expected_growth_factor);
    }
}

struct DetectBoundaryCase {
    const char* name;
    std::vector<CacheMeasurer::MeasurementResult> results;
    size_t expected_index;
    double expected_baseline_value;
};

TEST(CacheMeasurerTableTest, DetectsBoundariesFromTable) {
    const CacheMeasurer measurer;
    const std::vector<DetectBoundaryCase> cases{
        {
            "insufficient_points",
            {
                {64 * 1024, 1.0},
                {128 * 1024, 1.1},
            },
            2,
            0.0,
        },
        {
            "boundary_detected",
            {
                {64 * 1024, 1.0},
                {96 * 1024, 1.0},
                {128 * 1024, 1.0},
                {256 * 1024, 1.7},
                {512 * 1024, 2.0},
            },
            4,
            1.0,
        },
        {
            "no_boundary",
            {
                {64 * 1024, 1.0},
                {96 * 1024, 1.02},
                {128 * 1024, 1.01},
                {256 * 1024, 1.1},
            },
            4,
            1.01,
        },
    };

    for (const auto& test_case : cases) {
        SCOPED_TRACE(test_case.name);
        const auto boundary = measurer.detect_latency_boundary(test_case.results);
        EXPECT_EQ(boundary.index, test_case.expected_index);
        EXPECT_NEAR(boundary.baseline_value, test_case.expected_baseline_value, 1e-9);
    }
}

struct BoundaryCase {
    const char* name;
    double growth_factor;
    size_t left;
    size_t right;
    size_t precision;
    double baseline_mean;
    size_t switch_point;
    size_t expected_boundary;
};

TEST(RefinementTest, RefinesBoundaryFromTable) {
    const CacheMeasurer measurer;
    const std::vector<BoundaryCase> cases{
        {"sharp_jump", 1.5, 64, 256, 1, 10.0, 160, 159},
        {"later_jump", 2.0, 128, 512, 2, 5.0, 300, 299},
    };

    for (const auto& test_case : cases) {
        SCOPED_TRACE(test_case.name);
        const size_t boundary = measurer.refine_boundary(
            test_case.left,
            test_case.right,
            test_case.precision,
            test_case.growth_factor,
            [&test_case](size_t size) {
                return size < test_case.switch_point ? test_case.baseline_mean : test_case.baseline_mean * 3.0;
            },
            test_case.baseline_mean
        );

        EXPECT_EQ(boundary, test_case.expected_boundary);
    }
};

}  // namespace
}  // namespace silicon_probe::cache
