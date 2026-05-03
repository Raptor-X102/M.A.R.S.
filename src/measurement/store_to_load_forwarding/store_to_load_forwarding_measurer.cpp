// measurement/store_to_load_forwarding/store_to_load_forwarding_measurer.cpp
#include "measurement/store_to_load_forwarding/store_to_load_forwarding_measurer.hpp"

#include <numeric>

namespace silicon_probe::store_to_load_forwarding {

StoreToLoadForwardingMeasurer::StoreToLoadForwardingMeasurer() : StoreToLoadForwardingMeasurer(Config{}) {}

StoreToLoadForwardingMeasurer::StoreToLoadForwardingMeasurer(Config config) : config_(std::move(config)) {
    SPDLOG_INFO("[{}] cfg: offsets={}..{} step={} iter={} repeats={} growth={}", name(), config_.min_offset,
                config_.max_offset, config_.offset_step, config_.iterations, config_.repeats,
                config_.time_growth_ratio);
}

std::string_view StoreToLoadForwardingMeasurer::name() const noexcept {
    return "store-to-load forwarding";
}

void StoreToLoadForwardingMeasurer::measure(shared_types::CpuInfoData& data) {
    SPDLOG_INFO("[{}] start", name());
    platform::ScopedMeasurementEnvironment env{config_.environment};

    auto events = platform::discover_s2l_forwarding_events(data);
    bool has_pmc = !events.empty();
    std::unique_ptr<platform::pmc::PmcGroup> pmc;
    size_t sf_idx = std::string::npos;

    if (has_pmc) {
        pmc = platform::pmc::PmcGroup::create_raw(events);
        if (!pmc) {
            SPDLOG_WARN("[{}] pmc open failed, fallback to time-only", name());
            has_pmc = false;
        } else {
            for (size_t i = 0; i < events.size(); ++i)
                if (events[i].find("store_forward") != std::string::npos) sf_idx = i;
        }
    }

    size_t best_size = 0;
    size_t best_offset = 0;

    // Try sizes 8,4,2,1
    for (size_t size : {8, 4, 2, 1}) {
        if (size > kDefaultBufferSize) continue;
        SPDLOG_INFO("[{}] testing access size = {} bytes", name(), size);

        size_t max_off = std::min(config_.max_offset, kDefaultBufferSize - size);
        std::vector<StoreToLoadForwardingResult> results;
        std::vector<size_t> offsets;

        // Sweep all offsets from min to max
        for (size_t off = config_.min_offset; off <= max_off; off += config_.offset_step) {
            StoreToLoadForwardingResult res;
            switch (size) {
                case 8: res = run_test<8>(off, pmc.get(), events); break;
                case 4: res = run_test<4>(off, pmc.get(), events); break;
                case 2: res = run_test<2>(off, pmc.get(), events); break;
                case 1: res = run_test<1>(off, pmc.get(), events); break;
                default: __builtin_unreachable();
            }
            results.push_back(res);
            offsets.push_back(off);
        }
        if (results.empty()) continue;

        // Check if forwarding works at offset 0
        bool zero_works = false;
        if (has_pmc && sf_idx != std::string::npos && results[0].avg_events.size() > sf_idx) {
            double ratio = double(results[0].avg_events[sf_idx]) / config_.iterations;
            if (ratio < config_.pmc_saturation_ratio) zero_works = true;
        } else {
            if (max_off > 0 && results[0].avg_ticks < results.back().avg_ticks * config_.time_growth_ratio)
                zero_works = true;
        }

        if (!zero_works) {
            SPDLOG_INFO("[{}] size {} no STLF at offset 0, try smaller", name(), size);
            continue;
        }

        // Size works at offset 0 -> find max working offset
        best_size = size;
        best_offset = 0;
        double base_ticks = results[0].avg_ticks;

        for (size_t i = 1; i < results.size(); ++i) {
            bool ok = false;
            if (has_pmc && sf_idx != std::string::npos && results[i].avg_events.size() > sf_idx) {
                double ratio = double(results[i].avg_events[sf_idx]) / config_.iterations;
                if (ratio < config_.pmc_saturation_ratio) ok = true;
            } else {
                if (results[i].avg_ticks < base_ticks * config_.time_growth_ratio) ok = true;
            }
            if (ok)
                best_offset = offsets[i];
            else
                break;
        }

        SPDLOG_INFO("[{}] size {} works, max offset = {}", name(), best_size, best_offset);
        break;  // largest working size found
    }

    data.s2l_fwd_max_size = best_size;
    data.s2l_fwd_max_offset = best_offset;
    SPDLOG_INFO("[{}] result: size={} bytes, max_offset={}", name(), best_size, best_offset);
}

template <size_t N>
StoreToLoadForwardingResult StoreToLoadForwardingMeasurer::run_test(size_t offset, platform::pmc::PmcGroup* pmc,
                                                                   const std::vector<std::string>& ev_names) {
    static_assert(N == 1 || N == 2 || N == 4 || N == 8, "size must be 1,2,4,8");
    constexpr size_t buffer_bytes = kDefaultBufferSize;
    alignas(64) char buffer[buffer_bytes];
    std::memset(buffer, 0, buffer_bytes);
    volatile uint64_t dummy = 0;

    std::vector<double> ticks_vec;
    std::vector<std::vector<uint64_t>> all_counts;

    using StoreT = typename std::conditional<
        N == 1, uint8_t,
        typename std::conditional<N == 2, uint16_t,
                                  typename std::conditional<N == 4, uint32_t, uint64_t>::type>::type>::type;
    volatile StoreT* store_ptr = reinterpret_cast<volatile StoreT*>(buffer);
    volatile StoreT* load_ptr = reinterpret_cast<volatile StoreT*>(buffer + offset);

    for (size_t i = 0; i < config_.warmup_iterations; ++i) {
        *store_ptr = static_cast<StoreT>(i);
        asm volatile("" : : : "memory");
        dummy = *load_ptr;
        asm volatile("" : : : "memory");
    }

    for (size_t r = 0; r < config_.repeats; ++r) {
        if (pmc) {
            pmc->reset();
            pmc->enable();
        }

        uint64_t start = platform::arch::tick();
        for (size_t iter = 0; iter < config_.iterations; ++iter) {
            *store_ptr = static_cast<StoreT>(iter);
            asm volatile("" : : : "memory");
            dummy = *load_ptr;
            asm volatile("" : : : "memory");
        }
        uint64_t end = platform::arch::tick();

        if (pmc) pmc->disable();

        uint64_t total = end - start;
        ticks_vec.push_back(static_cast<double>(total) / config_.iterations);

        if (pmc) {
            auto cv = pmc->read();
            all_counts.push_back(std::move(cv.values));
        }
    }

    (void)dummy;

    double avg = std::accumulate(ticks_vec.begin(), ticks_vec.end(), 0.0) / config_.repeats;
    double stddev = 0.0;
    for (double v : ticks_vec) {
        double d = v - avg;
        stddev += d * d;
    }
    stddev = std::sqrt(stddev / config_.repeats);

    std::vector<uint64_t> avg_ev;
    if (!all_counts.empty()) {
        avg_ev.resize(all_counts[0].size(), 0);
        for (const auto& cnt : all_counts)
            for (size_t i = 0; i < cnt.size(); ++i) avg_ev[i] += cnt[i];
        for (size_t i = 0; i < avg_ev.size(); ++i) avg_ev[i] /= config_.repeats;
    }

    SPDLOG_INFO("[{}] \nsize={} off={}: avg={:.3g} std={:.3g}", name(), N, offset, avg, stddev);
    if (!avg_ev.empty()) {
        for (size_t i = 0; i < ev_names.size(); ++i)
            SPDLOG_INFO(" \n{} avg = {:.3g}", ev_names[i], double(avg_ev[i]));
    }

    return {avg, stddev, std::move(avg_ev)};
}

// Explicit template instantiations
template StoreToLoadForwardingResult StoreToLoadForwardingMeasurer::run_test<1>(
    size_t offset, platform::pmc::PmcGroup* pmc, const std::vector<std::string>& ev_names);
template StoreToLoadForwardingResult StoreToLoadForwardingMeasurer::run_test<2>(
    size_t offset, platform::pmc::PmcGroup* pmc, const std::vector<std::string>& ev_names);
template StoreToLoadForwardingResult StoreToLoadForwardingMeasurer::run_test<4>(
    size_t offset, platform::pmc::PmcGroup* pmc, const std::vector<std::string>& ev_names);
template StoreToLoadForwardingResult StoreToLoadForwardingMeasurer::run_test<8>(
    size_t offset, platform::pmc::PmcGroup* pmc, const std::vector<std::string>& ev_names);

}  // namespace silicon_probe::store_to_load_forwarding
