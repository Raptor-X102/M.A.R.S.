#pragma once

#include "silicon_probe/infra/logging.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

namespace silicon_probe::cache {

struct Statistics {
    double mean = 0.0;
    double stddev = 0.0;
};

struct BoundaryAnalyzerConfig {
    double growth_factor = 2.0;
    int test_samples = 3;
};

class BoundaryAnalyzer {
private:
    BoundaryAnalyzerConfig config_;

public:
    explicit BoundaryAnalyzer(BoundaryAnalyzerConfig config = {})
        : config_(config) {}

    Statistics compute_stats(const std::vector<double>& samples) const {
        Statistics statistics{};
        if (samples.empty()) {
            return statistics;
        }

        if (samples.size() == 1) {
            statistics.mean = samples.front();
            return statistics;
        }

        const double sum = std::accumulate(samples.begin(), samples.end(), 0.0);
        statistics.mean = sum / static_cast<double>(samples.size());

        double squared_sum = 0.0;
        for (const double sample : samples) {
            squared_sum += (sample - statistics.mean) * (sample - statistics.mean);
        }
        statistics.stddev = std::sqrt(squared_sum / static_cast<double>(samples.size() - 1));
        return statistics;
    }

    template <typename MeasureFn>
    size_t refine_boundary(size_t left, size_t right, size_t precision, MeasureFn&& measure, double baseline_mean) const {
        SPDLOG_DEBUG("[boundary] baseline={}, threshold={}x", baseline_mean, config_.growth_factor);

        size_t current_left = left;
        size_t current_right = right;

        while (current_right - current_left > precision) {
            const size_t midpoint = current_left + (current_right - current_left) / 2;

            std::vector<double> samples;
            samples.reserve(static_cast<size_t>(std::max(1, config_.test_samples)));
            for (int index = 0; index < config_.test_samples; ++index) {
                samples.push_back(measure(midpoint));
            }

            const auto statistics = compute_stats(samples);
            const double ratio = baseline_mean > 0.0 ? statistics.mean / baseline_mean : 0.0;
            const bool out_of_cache = ratio > config_.growth_factor;

            SPDLOG_DEBUG("[boundary] size={}, mean={}, ratio={}, threshold={}, decision={}",
                         midpoint,
                         statistics.mean,
                         ratio,
                         config_.growth_factor,
                         out_of_cache ? "out" : "in");

            if (out_of_cache) {
                current_right = midpoint;
            } else {
                current_left = midpoint;
            }
        }

        const size_t boundary = (current_left + current_right) / 2;
        SPDLOG_DEBUG("[boundary] final={} bytes", boundary);
        return boundary;
    }
};

} // namespace silicon_probe::cache
