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
    statistics.mean = sum / static_cast<double>(samples.size());

    double squared_sum = 0.0;
    for (const double sample : samples) {
        squared_sum += (sample - statistics.mean) * (sample - statistics.mean);
    }
    statistics.stddev = std::sqrt(squared_sum / static_cast<double>(samples.size() - 1));
    return statistics;
}

}  // namespace silicon_probe::cache
