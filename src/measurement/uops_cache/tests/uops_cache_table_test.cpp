#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <string>
#include <vector>

#define private public
#include "measurement/uops_cache/uops_cache_measurer.hpp"
#undef private

#include <gtest/gtest.h>

namespace silicon_probe::uops_cache {
namespace {

struct UopsCase {
    const char* name;
    std::vector<size_t> counts;
    std::vector<UopsCacheResult> results;
    std::vector<std::string> events;
    size_t expected_approx_saturation;
};

TEST(UopsCacheTableTest, FindsApproximateSaturationFromTable) {
    const std::vector<UopsCase> cases{
        {
            "largest_dsb_drop",
            {1200, 1300, 1400, 1500, 1600},
            {
                {1.0, 1000.0, 0.1, {10, 90}},
                {1.0, 1000.0, 0.1, {12, 88}},
                {1.0, 1000.0, 0.1, {15, 85}},
                {1.0, 1000.0, 0.1, {60, 40}},
                {1.0, 1000.0, 0.1, {65, 35}},
            },
            {"mite_uops", "dsb_uops"},
            1400,
        },
        {
            "missing_required_events",
            {1200, 1300, 1400},
            {
                {1.0, 1000.0, 0.1, {10, 90}},
                {1.0, 1000.0, 0.1, {20, 80}},
                {1.0, 1000.0, 0.1, {30, 70}},
            },
            {"other_event", "another_event"},
            0,
        },
    };

    for (const auto& test_case : cases) {
        SCOPED_TRACE(test_case.name);
        UopsCacheMeasurer measurer;
        EXPECT_EQ(measurer.findApproxSaturation(test_case.counts, test_case.results, test_case.events),
                  test_case.expected_approx_saturation);
    }
}

}  // namespace
}  // namespace silicon_probe::uops_cache
