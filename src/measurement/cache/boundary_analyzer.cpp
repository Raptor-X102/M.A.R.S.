// measurement/cache/boundary_analyzer.cpp
#include "measurement/cache/boundary_analyzer.hpp"

#include <cmath>
#include <numeric>

namespace silicon_probe::cache {

BoundaryAnalyzer::BoundaryAnalyzer(BoundaryAnalyzerConfig config) : config_(config) {}

Statistics BoundaryAnalyzer::compute_stats(const std::vector<double>& samples) {
    Statistics statistics{};
    if (samples.empty()) {
        return statistics;
    }

    if (samples.size() == 1) {
        statistics.mean = samples.front();
        return statistics;
    }

    const double sum = std::accumulate(samples.begin(), samples.end(), 0.0);
    statistics.mean  = sum / static_cast<double>(samples.size());

    double squared_sum = 0.0;
    for (const double sample : samples) {
        squared_sum += (sample - statistics.mean) * (sample - statistics.mean);
    }
    statistics.stddev = std::sqrt(squared_sum / static_cast<double>(samples.size() - 1));
    return statistics;
}

double BoundaryAnalyzer::compute_median(std::vector<double> samples) {
    if (samples.empty()) return 0.0;
    
    const size_t n = samples.size();
    const size_t mid = n / 2;
    
    std::nth_element(samples.begin(), samples.begin() + mid, samples.end());
    const double lower_median = samples[mid];
    
    if (n % 2 == 1) {
        return lower_median;
    }
    
    std::nth_element(samples.begin(), samples.begin() + mid - 1, samples.begin() + mid);
    const double upper_median = samples[mid - 1];
    
    return (lower_median + upper_median) / 2.0;
}

}  // namespace silicon_probe::cache
