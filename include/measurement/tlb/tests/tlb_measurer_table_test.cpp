#include <new>
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
    const char*         name;
    size_t              max_pages;
    std::vector<size_t> expected_counts;
};

TEST(TlbMeasurerTableTest, BuildsPageCountsFromTable) {
    const std::vector<BuildPageCountsCase> cases{
        {"power_of_two_tail", 64, {1, 2, 4, 8, 16, 32, 64}},
        {"non_power_of_two_tail", 40, {1, 2, 4, 8, 16, 32, 40}},
        {"custom_tail_cap", 48, {1, 2, 4, 8, 16, 32, 48}},
    };

    for (const auto& test_case : cases) {
        SCOPED_TRACE(test_case.name);

        TlbMeasurer::Config config{};
        config.max_pages = test_case.max_pages;

        const TlbMeasurer measurer(config);
        EXPECT_EQ(measurer.build_page_counts(), test_case.expected_counts);
    }
}

struct MeanFirstPointsCase {
    const char*          name;
    size_t               max_count;
    std::vector<Point>   points;
    double               expected;
};

TEST(TlbMeasurerTableTest, ComputesMeanOfFirstPointsFromTable) {
    const std::vector<MeanFirstPointsCase> cases{
        {
            "single_point",
            1,
            {
                {1, 4096, 1.0, 1.0, 1.0, 1.0},
                {2, 8192, 2.0, 2.0, 2.0, 2.0},
                {4, 16384, 3.0, 3.0, 3.0, 3.0},
            },
            1.0,
        },
        {
            "first_three_points",
            3,
            {
                {1, 4096, 0.0, 1.0, 0.0, 0.0},
                {2, 8192, 0.0, 2.0, 0.0, 0.0},
                {4, 16384, 0.0, 5.0, 0.0, 0.0},
                {8, 32768, 0.0, 8.0, 0.0, 0.0},
            },
            8.0 / 3.0,
        },
    };

    for (const auto& test_case : cases) {
        SCOPED_TRACE(test_case.name);

        EXPECT_NEAR(TlbMeasurer::mean_first_points(test_case.points, test_case.max_count), test_case.expected, 1e-9);
    }
}

struct PageNodeOffsetCase {
    const char* name;
    size_t      page_index;
    size_t      page_size_bytes;
    size_t      cache_line_bytes;
    size_t      expected_offset;
};

TEST(TlbMeasurerTableTest, DistributesPageNodesAcrossCacheLines) {
    const std::vector<PageNodeOffsetCase> cases{
        {"first_page_first_line", 0, 4096, 64, 0},
        {"second_page_second_line", 1, 4096, 64, 64},
        {"sixty_fourth_page_last_line", 63, 4096, 64, 63 * 64},
        {"sixty_fifth_page_wraps", 64, 4096, 64, 0},
        {"large_stride_uses_cache_line_size", 2, 4096, 128, 256},
    };

    constexpr size_t page_count = 96;
    constexpr size_t page_size  = 4096;
    void* const base            = ::operator new(page_count * page_size, std::align_val_t(64));

    for (const auto& test_case : cases) {
        SCOPED_TRACE(test_case.name);

        const auto nodes =
            TlbMeasurer::make_page_nodes(base, page_count, test_case.page_size_bytes, test_case.cache_line_bytes);
        const auto* bytes      = static_cast<std::byte*>(base);
        const auto* node_bytes = reinterpret_cast<const std::byte*>(nodes.at(test_case.page_index));
        const size_t offset =
            static_cast<size_t>(node_bytes - bytes - test_case.page_index * test_case.page_size_bytes);

        EXPECT_EQ(offset, test_case.expected_offset);
    }

    ::operator delete(base, std::align_val_t(64));
}

struct DetectBoundaryCase {
    const char*           name;
    std::vector<Point>    points;
    std::optional<size_t> expected_l1_index;
    std::optional<size_t> expected_l2_index;
};

TEST(TlbMeasurerTableTest, DetectsBoundariesFromTable) {
    const std::vector<DetectBoundaryCase> cases{
        {
            "three_level_curve",
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
            7,
        },
        {
            "flat_curve",
            {
                {16, 16 * 4096, 0.0, 1.00, 0.0, 0.0},
                {32, 32 * 4096, 0.0, 1.03, 0.0, 0.0},
                {64, 64 * 4096, 0.0, 1.04, 0.0, 0.0},
                {128, 128 * 4096, 0.0, 1.05, 0.0, 0.0},
            },
            std::nullopt,
            std::nullopt,
        },
    };

    for (const auto& test_case : cases) {
        SCOPED_TRACE(test_case.name);

        const TlbMeasurer measurer;
        const auto detection = measurer.detect_boundaries(test_case.points);

        EXPECT_EQ(detection.l1, test_case.expected_l1_index);
        EXPECT_EQ(detection.l2, test_case.expected_l2_index);
    }
}

} // namespace
} // namespace silicon_probe::tlb
