#pragma once

#include <vector>
#include <cmath>
#include <optional>
#include <numeric>
#include <iostream>
#include <algorithm>

struct MeasurementPoint {
    size_t size_bytes;
    double cycles_per_element;
};

struct Statistics {
    double mean;
    double stddev;
    
    Statistics() : mean(0.0), stddev(0.0) {}
};

struct BoundaryAnalyzerConfig {
    double growth_factor = 2.0;           // во сколько раз может вырасти latency, оставаясь в кэше
    int baseline_samples = 5;
    int test_samples = 3;
};

class BoundaryAnalyzer {
public:
    explicit BoundaryAnalyzer(const BoundaryAnalyzerConfig& cfg = BoundaryAnalyzerConfig{}) 
        : config_(cfg) {}
    
    Statistics compute_stats(const std::vector<double>& samples) const {
        Statistics stats;
        if (samples.size() < 2) {
            if (samples.size() == 1) {
                stats.mean = samples[0];
                stats.stddev = 0.0;
            }
            return stats;
        }
        
        double sum = std::accumulate(samples.begin(), samples.end(), 0.0);
        stats.mean = sum / samples.size();
        
        double sq_sum = 0.0;
        for (double v : samples) {
            sq_sum += (v - stats.mean) * (v - stats.mean);
        }
        stats.stddev = std::sqrt(sq_sum / (samples.size() - 1));
        
        return stats;
    }
    
    bool is_out_of_cache(double value, double baseline_mean) const {
        if (value <= baseline_mean) return false;
        
        double ratio = value / baseline_mean;
        bool result = ratio > config_.growth_factor;
        
        std::cout << "    ratio=" << ratio 
                  << ", threshold=" << config_.growth_factor
                  << " => " << (result ? "OUT" : "IN") << std::endl;
        
        return result;
    }

    template<typename F>
    size_t refine_boundary(size_t left, size_t right, size_t precision, F measure_func,
                          double baseline_mean) {
        
        std::cout << "[Boundary] Fixed baseline mean=" << baseline_mean 
                  << ", growth threshold=" << config_.growth_factor << "x" << std::endl;
        
        size_t current_left = left;
        size_t current_right = right;
        
        while (current_right - current_left > precision) {
            size_t mid = current_left + (current_right - current_left) / 2;
            
            std::vector<double> test;
            for (int i = 0; i < config_.test_samples; ++i) {
                test.push_back(measure_func(mid));
            }
            
            auto test_stats = compute_stats(test);
            
            std::cout << "[Boundary] Size " << mid << ": mean=" << test_stats.mean;
            bool out = is_out_of_cache(test_stats.mean, baseline_mean);
            
            if (!out) {
                std::cout << "  --> IN cache (moving left)";
                current_left = mid;
            } else {
                std::cout << "  --> OUT of cache (moving right)";
                current_right = mid;
            }
            std::cout << std::endl;
        }
        
        return (current_left + current_right) / 2;
    }
    
private:
    BoundaryAnalyzerConfig config_;
};
