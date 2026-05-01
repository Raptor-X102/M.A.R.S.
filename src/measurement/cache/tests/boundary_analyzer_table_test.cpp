#include <gtest/gtest.h>
#include <vector>

#include "measurement/cache/boundary_analyzer.hpp"

namespace silicon_probe::cache {
namespace {

struct StatsCase {
    const char* name;
    std::vector<double> samples;
    double expected_mean;
    double expected_stddev;
};

TEST(BoundaryAnalyzerTableTest, ComputesStatsFromTable) {
    const BoundaryAnalyzer analyzer;
    const std::vector<StatsCase> cases{
        {"empty", {}, 0.0, 0.0},
        {"single", {4.0}, 4.0, 0.0},
        {"many", {2.0, 4.0, 4.0, 4.0, 5.0, 5.0, 7.0, 9.0}, 5.0, 2.13809},
    };

    for (const auto& test_case : cases) {
        SCOPED_TRACE(test_case.name);
        const Statistics statistics = analyzer.compute_stats(test_case.samples);
        EXPECT_DOUBLE_EQ(statistics.mean, test_case.expected_mean);
        EXPECT_NEAR(statistics.stddev, test_case.expected_stddev, 1e-5);
    }
}

struct BoundaryCase {
    const char* name;
    BoundaryAnalyzerConfig config;
    size_t left;
    size_t right;
    size_t precision;
    double baseline_mean;
    size_t switch_point;
    size_t expected_boundary;
};

TEST(BoundaryAnalyzerTableTest, RefinesBoundaryFromTable) {
    const std::vector<BoundaryCase> cases{
        {"sharp_jump", {1.5, 3}, 64, 256, 1, 10.0, 160, 159},
        {"later_jump", {2.0, 2}, 128, 512, 2, 5.0, 300, 299},
    };

    for (const auto& test_case : cases) {
        SCOPED_TRACE(test_case.name);
        const BoundaryAnalyzer analyzer(test_case.config);
        const size_t boundary = analyzer.refine_boundary(
            test_case.left, test_case.right, test_case.precision,
            [&test_case](size_t size) {
                return size < test_case.switch_point ? test_case.baseline_mean : test_case.baseline_mean * 3.0;
            },
            test_case.baseline_mean);

        EXPECT_EQ(boundary, test_case.expected_boundary);
    }
}

}  // namespace
}  // namespace silicon_probe::cache
