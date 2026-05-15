#pragma once

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

namespace silicon_probe::common::statistics {

struct Statistics {
    double mean = 0.0;
    double stddev = 0.0;   // sample standard deviation (unbiased)
    size_t cnt = 0;
};

// Mean of a vector
inline double mean(const std::vector<double>& samples) {
    if (samples.empty()) return 0.0;
    return std::accumulate(samples.begin(), samples.end(), 0.0) / samples.size();
}

// Sample standard deviation (unbiased, divisor = cnt-1)
// Returns 0.0 if cnt < 2
inline double sample_stddev(const std::vector<double>& samples) {
    const size_t cnt = samples.size();
    if (cnt < 2) return 0.0;
    double m = mean(samples);
    double sq_sum = 0.0;
    for (double x : samples) {
        double diff = x - m;
        sq_sum += diff * diff;
    }
    return std::sqrt(sq_sum / (cnt - 1));
}

// Population standard deviation (divisor = cnt)
inline double population_stddev(const std::vector<double>& samples) {
    const size_t cnt = samples.size();
    if (cnt == 0) return 0.0;
    double m = mean(samples);
    double sq_sum = 0.0;
    for (double x : samples) {
        double diff = x - m;
        sq_sum += diff * diff;
    }
    return std::sqrt(sq_sum / cnt);
}

// Compute both mean and sample stddev in one pass
inline Statistics compute_stats(const std::vector<double>& samples) {
    Statistics stats;
    stats.cnt = samples.size();
    if (stats.cnt == 0) return stats;
    if (stats.cnt == 1) {
        stats.mean = samples[0];
        stats.stddev = 0.0;
        return stats;
    }
    double sum = 0.0, sum_sq = 0.0;
    for (double x : samples) {
        sum += x;
        sum_sq += x * x;
    }
    stats.mean = sum / stats.cnt;
    double variance = (sum_sq - sum * sum / stats.cnt) / (stats.cnt - 1);
    stats.stddev = std::sqrt(variance);
    return stats;
}

// Median 
inline double compute_median(std::vector<double> samples) {
    if (samples.empty()) return 0.0;
    const size_t cnt = samples.size();
    const size_t mid = cnt / 2;
    std::nth_element(samples.begin(), samples.begin() + mid, samples.end());
    double median_val = samples[mid];
    if (cnt % 2 == 0) {
        // For even size, need average of two middle elements.
        // The left middle is at mid-1, but after nth_element, elements before mid are <= samples[mid].
        // We need the maximum of the left half.
        double left_max = *std::max_element(samples.begin(), samples.begin() + mid);
        median_val = (median_val + left_max) / 2.0;
    }
    return median_val;
}

}
