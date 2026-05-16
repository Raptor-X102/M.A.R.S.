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
    size_t expected_approx_saturation;
};

TEST(UopsCacheTableTest, FindsApproximateSaturationFromTable) {
    const std::vector<UopsCase> cases{
        {
            "largest_dsb_drop",
            {1200, 1300, 1400, 1500, 1600},
            {
                {1.0, 0.1, {10, 90}},   // mite=10, dsb=90 → share=0.9
                {1.0, 0.1, {12, 88}},   // share=0.88
                {1.0, 0.1, {15, 85}},   // share=0.85
                {1.0, 0.1, {60, 40}},   // share=0.4  (drop 0.45)
                {1.0, 0.1, {65, 35}},   // share=0.35
            },
            1400,  // index of the point before the largest drop
        },
        {
            "no_significant_drop",
            {1200, 1300, 1400},
            {
                {1.0, 0.1, {10, 90}},   // 0.9
                {1.0, 0.1, {20, 80}},   // 0.8 (drop 0.1)
                {1.0, 0.1, {30, 70}},   // 0.7 (drop 0.1)
            },
            0,  // max drop 0.1 < config_.dsb_drop_significant (0.2)
        },
    };

    for (const auto& test_case : cases) {
        SCOPED_TRACE(test_case.name);
        UopsCacheMeasurer measurer;
        EXPECT_EQ(
            measurer.findApproxSaturation(test_case.counts, test_case.results),
            test_case.expected_approx_saturation
        );
    }
}

}  // namespace
}  // namespace silicon_probe::uops_cache
