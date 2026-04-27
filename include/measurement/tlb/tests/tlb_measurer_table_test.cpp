#include <optional>
#include <vector>

#define private public
#include "measurement/tlb/tlb_measurer.hpp"
#undef private

#include <gtest/gtest.h>

namespace silicon_probe::tlb {
namespace {

using Point = core::TlbSummaryPoint;

struct BuildPageCountsCase {
    const char* name;
    TlbMeasurer::Config config;
    std::vector<size_t> expected_counts;
};

TEST(TlbMeasurerTableTest, BuildsPageCountsFromTable) {
    std::vector<BuildPageCountsCase> cases{};

    {
        TlbMeasurer::Config config{};
        config.min_pages = 1;
        config.max_pages = 64;
        config.pages_step = 2;
        config.growth_mode = GrowthMode::Multiply;
        cases.push_back({"multiply_power_of_two", config, {1, 2, 4, 8, 16, 32, 64}});
    }

    {
        TlbMeasurer::Config config{};
        config.min_pages = 3;
        config.max_pages = 40;
        config.pages_step = 10;
        config.growth_mode = GrowthMode::Add;
        cases.push_back({"linear_with_tail_cap", config, {3, 13, 23, 33, 40}});
    }

    {
        TlbMeasurer::Config config{};
        config.min_pages = 5;
        config.max_pages = 48;
        config.pages_step = 3;
        config.growth_mode = GrowthMode::Multiply;
        cases.push_back({"multiply_non_power_of_two", config, {5, 15, 45, 48}});
    }

    for (const auto& test_case : cases) {
        SCOPED_TRACE(test_case.name);
        const TlbMeasurer measurer(test_case.config);
        EXPECT_EQ(measurer.build_page_counts(), test_case.expected_counts);
    }
}

struct MovingAverageCase {
    const char* name;
    size_t window;
    std::vector<Point> points;
    std::vector<double> expected;
};

TEST(TlbMeasurerTableTest, ComputesMovingAverageFromTable) {
    const std::vector<MovingAverageCase> cases{
        {
            "window_1_identity",
            1,
            {
                {1, 4096, 1.0, 1.0, 1.0, 1.0},
                {2, 8192, 2.0, 2.0, 2.0, 2.0},
                {4, 16384, 3.0, 3.0, 3.0, 3.0},
            },
            {1.0, 2.0, 3.0},
        },
        {
            "window_3_centered",
            3,
            {
                {1, 4096, 0.0, 1.0, 0.0, 0.0},
                {2, 8192, 0.0, 2.0, 0.0, 0.0},
                {4, 16384, 0.0, 5.0, 0.0, 0.0},
                {8, 32768, 0.0, 8.0, 0.0, 0.0},
            },
            {1.5, 8.0 / 3.0, 5.0, 6.5},
        },
    };

    for (const auto& test_case : cases) {
        SCOPED_TRACE(test_case.name);
        TlbMeasurer::Config config{};
        config.detection.moving_average_window = test_case.window;

        const TlbMeasurer measurer(config);
        const auto actual = measurer.moving_average(test_case.points);

        ASSERT_EQ(actual.size(), test_case.expected.size());
        for (size_t index = 0; index < actual.size(); ++index) {
            EXPECT_NEAR(actual[index], test_case.expected[index], 1e-9);
        }
    }
}

struct DetectBoundaryCase {
    const char* name;
    TlbMeasurer::Config config;
    std::vector<Point> points;
    std::optional<size_t> expected_l1_index;
    std::optional<size_t> expected_l2_index;
    std::optional<size_t> expected_page_walk_index;
};

TEST(TlbMeasurerTableTest, DetectsBoundariesFromTable) {
    std::vector<DetectBoundaryCase> cases{};

    {
        TlbMeasurer::Config config{};
        config.detection.moving_average_window = 1;
        config.detection.l1_growth_ratio = 1.10;
        config.detection.l2_growth_ratio = 1.08;
        config.detection.page_walk_growth_ratio = 1.12;
        config.detection.min_jump_cycles = 0.75;
        config.detection.page_walk_jump_cycles = 2.0;
        config.detection.sustain_points = 1;

        cases.push_back({
            "three_level_curve",
            config,
            {
                {16, 16 * 4096, 0.0, 1.00, 0.0, 0.0},
                {32, 32 * 4096, 0.0, 1.02, 0.0, 0.0},
                {64, 64 * 4096, 0.0, 1.01, 0.0, 0.0},
                {128, 128 * 4096, 0.0, 2.10, 0.0, 0.0},
                {256, 256 * 4096, 0.0, 2.15, 0.0, 0.0},
                {512, 512 * 4096, 0.0, 3.05, 0.0, 0.0},
                {1024, 1024 * 4096, 0.0, 3.12, 0.0, 0.0},
                {2048, 2048 * 4096, 0.0, 5.60, 0.0, 0.0},
                {4096, 4096 * 4096, 0.0, 5.80, 0.0, 0.0},
            },
            3,
            5,
            7,
        });
    }

    {
        TlbMeasurer::Config config{};
        config.detection.moving_average_window = 1;
        config.detection.min_jump_cycles = 0.75;
        config.detection.page_walk_jump_cycles = 2.0;

        cases.push_back({
            "flat_curve",
            config,
            {
                {16, 16 * 4096, 0.0, 1.00, 0.0, 0.0},
                {32, 32 * 4096, 0.0, 1.03, 0.0, 0.0},
                {64, 64 * 4096, 0.0, 1.04, 0.0, 0.0},
                {128, 128 * 4096, 0.0, 1.05, 0.0, 0.0},
            },
            std::nullopt,
            std::nullopt,
            std::nullopt,
        });
    }

    for (const auto& test_case : cases) {
        SCOPED_TRACE(test_case.name);
        const TlbMeasurer measurer(test_case.config);
        const auto detection = measurer.detect_boundaries(test_case.points);
        EXPECT_EQ(detection.l1_index, test_case.expected_l1_index);
        EXPECT_EQ(detection.l2_index, test_case.expected_l2_index);
        EXPECT_EQ(detection.page_walk_index, test_case.expected_page_walk_index);
    }
}

} // namespace
} // namespace silicon_probe::tlb
