// measurement/tlb/tlb_measurer.hpp
#pragma once

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <numeric>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "core/measurer.hpp"
#include "infra/logging.hpp"
#include "platform/arch.hpp"
#include "platform/os.hpp"

namespace silicon_probe::tlb {

class TlbMeasurer final : public core::Measurer {
public:
    static constexpr size_t kMinPages = 1;
    static constexpr size_t kDefaultMaxPages = 4096;
    static constexpr size_t kPagesStep = 2;
    static constexpr size_t kDefaultIterations = 2'000'000;
    static constexpr size_t kRepeats = 9;
    static constexpr size_t kWarmupRounds = 2;
    static constexpr size_t kDefaultPageSize = 4 * 1024;
    static constexpr size_t kHugePageSize = 2 * 1024 * 1024;
    static constexpr size_t kPagePoolScale = 8;
    static constexpr unsigned int kSeed = 0xC0FFEEU;
    static constexpr size_t kMinL1CandidatePages = 64;
    static constexpr double kL1GrowthRatio = 1.10;
    static constexpr double kL2GrowthRatio = 1.20;
    static constexpr double kL1MinJumpCycles = 0.50;
    static constexpr double kL2MinJumpCycles = 1.00;
    static constexpr size_t kL2SearchGapPoints = 2;
    static constexpr bool kLockMemory = false;
    static constexpr bool kDisableThpFor4KPages = true;

    struct Config {
        bool enabled = true;
        size_t max_pages = kDefaultMaxPages;
        size_t iterations = kDefaultIterations;
        bool use_huge_pages = false;
        platform::MeasurementEnvironmentOptions environment;
    };

    TlbMeasurer();
    explicit TlbMeasurer(Config config);

    std::string_view name() const noexcept override;
    void measure(shared_types::CpuInfoData& data) override;

private:
    struct PageNode {
        PageNode* next = nullptr;
        std::uint64_t tag = 0;
    } __attribute__((aligned(64)));

    struct Mapping {
        void* base = nullptr;
        size_t size_bytes = 0;
        bool huge = false;
        bool locked = false;

        Mapping() = default;
        Mapping(const Mapping&) = delete;
        Mapping& operator=(const Mapping&) = delete;
        Mapping(Mapping&& other) noexcept;
        Mapping& operator=(Mapping&& other) noexcept;
        ~Mapping();
        void release() noexcept;
    };

    struct Boundaries {
        std::optional<size_t> l1;
        std::optional<size_t> l2;
    };

    Config config_;

    template <typename T>
    static void compiler_barrier(const T& value);
    size_t page_size_bytes() const noexcept;
    void validate_config() const;
    static size_t checked_mul(size_t lhs, size_t rhs);
    std::vector<size_t> build_page_counts() const;
    Mapping allocate_mapping(size_t size_bytes) const;
    void advise_mapping(const Mapping& mapping) const;
    static std::vector<PageNode*> make_page_nodes(void* base, size_t page_count, size_t page_size,
                                                  size_t cache_line_bytes);
    static void pretouch(const std::vector<PageNode*>& nodes);
    static void link_ring(const std::vector<PageNode*>& order, size_t count);
    void warm_instruction_path(PageNode* start);
    static void warmup(PageNode* start, size_t pages);
    std::uint64_t measure_cycles(PageNode* start) const;
    shared_types::TlbSummaryPoint measure_point(std::vector<PageNode*>& pool, std::vector<PageNode*>& order,
                                                std::mt19937& rng, size_t pages, size_t page_size) const;
    static Boundaries detect_boundaries(const std::vector<shared_types::TlbSummaryPoint>& points);
    static double mean_first_points(const std::vector<shared_types::TlbSummaryPoint>& points, size_t max_count);
    static size_t first_index_with_at_least_pages(const std::vector<shared_types::TlbSummaryPoint>& points,
                                                  size_t pages);
    static std::optional<size_t> find_latency_jump(const std::vector<shared_types::TlbSummaryPoint>& points,
                                                   size_t start_index, double reference_level,
                                                   double ratio_threshold, double min_jump_cycles);
    static bool jump_is_sustained(const std::vector<shared_types::TlbSummaryPoint>& points, size_t index);
    static double median(const std::vector<double>& sorted_values);
};

}  // namespace silicon_probe::tlb
