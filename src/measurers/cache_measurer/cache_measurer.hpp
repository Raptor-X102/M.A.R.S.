#pragma once

#include <vector>
#include <optional>
#include <cmath>
#include <bitset>
#include "measurer.hpp"
#include "cache_profiler_list.hpp"
#include "os.hpp"
#include "arch.hpp"
#include "logger.hpp"

enum class CacheLevel {
    L1D = 0,
    L2  = 1,
    L3  = 2
};

struct BoundaryResult {
    size_t index;    
    double baseline_latency;  
};

class CacheMeasurer : public Measurer {
public:
    static constexpr double LATENCY_TRESHOLD = 2;
    static constexpr size_t L1_MAX_SIZE = 128 * 1024;
    static constexpr size_t L2_MAX_SIZE = 2 * 1024 * 1024;
    static constexpr size_t L3_MAX_SIZE = 128 * 1024 * 1024;
    static constexpr size_t SIZE_WITH_DEF_ITER_SIZE = L3_MAX_SIZE / 512;
    static constexpr size_t CACHE_MIN_SIZE = 16; // in cache lines, 16 * 64 = 1024
    static constexpr size_t DEF_SEED = 0xBEAF;
    static constexpr size_t DEF_WARMUP_ITERATIONS = 4;
    static constexpr size_t DEF_MEASURE_ITERATIONS = 8 * 1024;
    static constexpr size_t DEF_PRECISION = 64;
    static constexpr double L1_GROWTH_FACTOR = 1.5;
    static constexpr double L2_GROWTH_FACTOR = 2.0;
    static constexpr double L3_GROWTH_FACTOR = 2.2;
    static constexpr int BASELINE_SAMPLES = 3;
    static constexpr double STABILITY_THRESHOLD = 0.2; // 20% max diff for baseline
    static constexpr size_t MAX_ITERATIONS = 1000;    // max iterations per measurement
    static constexpr size_t MIN_ITERATIONS = 1;       // min iterations per measurement
    static constexpr size_t TARGET_ACCESSES = 2'000'000; // target total memory accesses

    struct Config {
        std::bitset<3> levels;
        size_t l1_max;
        size_t l2_max;
        size_t l3_max;
        size_t cache_min_size;
        unsigned int seed;
        size_t warmup_iterations;
        size_t measure_iterations;
        size_t precision; // in bytes

        Config() 
            : levels(0b111)
            , l1_max(L1_MAX_SIZE)
            , l2_max(L2_MAX_SIZE)
            , l3_max(L3_MAX_SIZE)
            , cache_min_size(CACHE_MIN_SIZE) // in cache lines, 16 * 64 = 1024
            , seed(DEF_SEED)
            , warmup_iterations(DEF_WARMUP_ITERATIONS )
            , measure_iterations(DEF_MEASURE_ITERATIONS )
            , precision(DEF_PRECISION) {}

        static Config only_l1() {
            Config cfg;
            cfg.levels = 0b001;
            return cfg;
        }

        static Config only_l2() {
            Config cfg;
            cfg.levels = 0b010;
            return cfg;
        }

        static Config only_l3() {
            Config cfg;
            cfg.levels = 0b100;
            return cfg;
        }

        static Config l1_and_l2() {
            Config cfg;
            cfg.levels = 0b011;
            return cfg;
        }
    };

    explicit CacheMeasurer(const Config& config = Config{});
    
    void measure(CPUInfoData& data) override;
    std::string name() const override { return "CacheMeasurer"; }

    void measure_l1(CPUInfoData& data);
    void measure_l2(CPUInfoData& data);
    void measure_l3(CPUInfoData& data);

private:
    struct MeasurementResult {
        size_t size_bytes;
        double cycles_per_element;
        
        MeasurementResult(size_t size, double cpe) 
            : size_bytes(size), cycles_per_element(cpe) {}

        MeasurementResult(const MeasurementResult&) = delete;
        MeasurementResult& operator=(const MeasurementResult&) = delete;
    
        MeasurementResult(MeasurementResult&&) = default;
        MeasurementResult& operator=(MeasurementResult&&) = default;
    };
    
    

    std::vector<MeasurementResult> measure_range(size_t min_size, size_t max_size);
    MeasurementResult do_single_measurement(size_t size);
    
    BoundaryResult 
    detect_boundary(const std::vector<MeasurementResult>& results) const;

    size_t refine_boundary(const std::vector<MeasurementResult>& results,
                           BoundaryResult& result);
    void flush_cache_and_warmup(CacheProfilerList& list, size_t count);

    Config config_;
    size_t cache_line_size_;
};
