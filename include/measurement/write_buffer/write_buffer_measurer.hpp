// measurement/write_buffer/write_buffer_measurer.hpp
#pragma once

#include <cmath>
#include <cstring>
#include <memory>
#include <numeric>

#include "core/measurer.hpp"
#include "infra/logging.hpp"
#include "platform/arch.hpp"
#include "platform/events_discovery.hpp"
#include "platform/os.hpp"
#include "platform/pmc.hpp"

namespace silicon_probe::write_buffer {

struct WriteBufferResult {
    double avg_latency_ticks;
    double latency_stddev;
    std::vector<uint64_t> avg_events;
};

class WriteBufferMeasurer final : public core::Measurer {
   public:
    static constexpr size_t kDefaultMaxWrites        = 64;
    static constexpr size_t kDefaultMinWrites        = 1;
    static constexpr size_t kDefaultWritesStep       = 1;
    static constexpr size_t kDefaultIterations       = 5000;
    static constexpr size_t kDefaultRepeats          = 30;
    static constexpr size_t kDefaultWarmupIterations = 5;
    static constexpr size_t kBufferSizeMB            = 16;
    static constexpr size_t kBytesPerEntry           = 4;
    static constexpr size_t kCacheLineSize           = 64;
    static constexpr size_t kStride = kCacheLineSize / kBytesPerEntry;   // = 16

    struct Config {
        bool enabled = true;
        platform::MeasurementEnvironmentOptions environment;
        size_t max_writes        = kDefaultMaxWrites;
        size_t min_writes        = kDefaultMinWrites;
        size_t writes_step       = kDefaultWritesStep;
        size_t iterations        = kDefaultIterations;
        size_t repeats           = kDefaultRepeats;
        size_t warmup_iterations = kDefaultWarmupIterations;

        double latency_spike_ratio  = 2.0;   // latency > baseline * this → overflow
        double latency_hold_ratio   = 1.5;   // for follow-up check after spike
        double stall_fallback_ratio = 0.9;   // if no latency spike, use stalls > max_stalls * this
        size_t baseline_window      = 3;     // number of initial points for baseline latency
        double stall_baseline_ratio = 10.0;    // stalls > baseline_stalls * this -> overflow
        double stall_absolute_min = 100.0;     // absolute minimum stalls threshold
        double stall_gradient_ratio = 10.0;     // (stalls[i] - stalls[i-1]) / stalls[i-1] > this -> overflow
        size_t stall_median_window = 3;
    };

    WriteBufferMeasurer();
    explicit WriteBufferMeasurer(Config config);

    std::string_view name() const noexcept override;
    void measure(shared_types::CpuInfoData& data) override;

   private:
    Config config_;

    void validateConfig();
    WriteBufferResult measure_for_writes(
        size_t num_writes,
        int* fill_base,
        volatile int* extra_addr,
        volatile int& dummy,
        platform::pmc::PmcGroup* pmc
    );
    size_t analyze_buffer_capacity(
        const std::vector<WriteBufferResult>& results,
        const std::vector<size_t>& writes_list,
        size_t sb_idx,
        size_t bound_idx
    ) const;
};

}  // namespace silicon_probe::write_buffer
