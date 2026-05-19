#pragma once

#include <algorithm>
#include <cmath>
#include <numeric>
#include <type_traits>
#include <vector>

namespace silicon_probe::common::statistics {

struct Statistics {
    double mean   = 0.0;
    double stddev = 0.0;  // sample standard deviation (unbiased)
    size_t cnt    = 0;
};

// ---- Overloads for arithmetic containers (integral or floating) ----
// Convert to double internally

template <typename T>
inline double mean(const std::vector<T>& samples) {
    static_assert(std::is_arithmetic_v<T>, "mean requires arithmetic type");
    if (samples.empty())
        return 0.0;
    double sum = 0.0;
    for (T x : samples)
        sum += static_cast<double>(x);
    return sum / samples.size();
}

template <typename T>
inline double sample_stddev(const std::vector<T>& samples) {
    static_assert(std::is_arithmetic_v<T>, "sample_stddev requires arithmetic type");
    const size_t cnt = samples.size();
    if (cnt < 2)
        return 0.0;
    double m      = mean(samples);
    double sq_sum = 0.0;
    for (T x : samples) {
        double diff = static_cast<double>(x) - m;
        sq_sum += diff * diff;
    }
    return std::sqrt(sq_sum / (cnt - 1));
}

template <typename T>
inline double population_stddev(const std::vector<T>& samples) {
    static_assert(std::is_arithmetic_v<T>, "population_stddev requires arithmetic type");
    const size_t cnt = samples.size();
    if (cnt == 0)
        return 0.0;
    double m      = mean(samples);
    double sq_sum = 0.0;
    for (T x : samples) {
        double diff = static_cast<double>(x) - m;
        sq_sum += diff * diff;
    }
    return std::sqrt(sq_sum / cnt);
}

template <typename T>
inline Statistics compute_stats(const std::vector<T>& samples) {
    static_assert(std::is_arithmetic_v<T>, "compute_stats requires arithmetic type");
    Statistics stats;
    stats.cnt = samples.size();
    if (stats.cnt == 0)
        return stats;
    if (stats.cnt == 1) {
        stats.mean   = static_cast<double>(samples[0]);
        stats.stddev = 0.0;
        return stats;
    }
    double sum = 0.0, sum_sq = 0.0;
    for (T x : samples) {
        double d = static_cast<double>(x);
        sum += d;
        sum_sq += d * d;
    }
    stats.mean      = sum / stats.cnt;
    double variance = (sum_sq - sum * sum / stats.cnt) / (stats.cnt - 1);
    stats.stddev    = std::sqrt(variance);
    return stats;
}

template <typename T>
inline double compute_median(std::vector<T> samples) {
    static_assert(std::is_arithmetic_v<T>, "compute_median requires arithmetic type");
    if (samples.empty())
        return 0.0;
    const size_t cnt = samples.size();
    const size_t mid = cnt / 2;
    std::nth_element(samples.begin(), samples.begin() + mid, samples.end());
    double median_val = static_cast<double>(samples[mid]);
    if (cnt % 2 == 0) {
        // For even size, need average of two middle elements.
        // The left middle is at mid-1; after nth_element, elements before mid are <= samples[mid].
        // We need the maximum of the left half.
        T left_max = *std::max_element(samples.begin(), samples.begin() + mid);
        median_val = (median_val + static_cast<double>(left_max)) / 2.0;
    }
    return median_val;
}

// ---- Original overloads for double (keep for compatibility) ----
// Mean of a vector<double>
inline double mean(const std::vector<double>& samples) {
    if (samples.empty())
        return 0.0;
    return std::accumulate(samples.begin(), samples.end(), 0.0) / samples.size();
}

// Sample standard deviation (unbiased, divisor = cnt-1)
inline double sample_stddev(const std::vector<double>& samples) {
    const size_t cnt = samples.size();
    if (cnt < 2)
        return 0.0;
    double m      = mean(samples);
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
    if (cnt == 0)
        return 0.0;
    double m      = mean(samples);
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
    if (stats.cnt == 0)
        return stats;
    if (stats.cnt == 1) {
        stats.mean   = samples[0];
        stats.stddev = 0.0;
        return stats;
    }
    double sum = 0.0, sum_sq = 0.0;
    for (double x : samples) {
        sum += x;
        sum_sq += x * x;
    }
    stats.mean      = sum / stats.cnt;
    double variance = (sum_sq - sum * sum / stats.cnt) / (stats.cnt - 1);
    stats.stddev    = std::sqrt(variance);
    return stats;
}

// Median
inline double compute_median(std::vector<double> samples) {
    if (samples.empty())
        return 0.0;
    const size_t cnt = samples.size();
    const size_t mid = cnt / 2;
    std::nth_element(samples.begin(), samples.begin() + mid, samples.end());
    double median_val = samples[mid];
    if (cnt % 2 == 0) {
        double left_max = *std::max_element(samples.begin(), samples.begin() + mid);
        median_val      = (median_val + left_max) / 2.0;
    }
    return median_val;
}

}  // namespace silicon_probe::common::statistics
