#pragma once

#include "infra/logging.hpp"
#include "measurement/core/measurer.hpp"
#include "platform/arch.hpp"
#include "platform/os.hpp"

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
#include <string_view>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

namespace silicon_probe::tlb {

class TlbMeasurer final : public core::Measurer {
  public:
    static constexpr size_t kMinPages             = 1;
    static constexpr size_t kDefaultMaxPages      = 4096;
    static constexpr size_t kPagesStep            = 2;
    static constexpr size_t kDefaultIterations    = 2'000'000;
    static constexpr size_t kRepeats              = 9;
    static constexpr size_t kWarmupRounds         = 2;
    static constexpr size_t kDefaultPageSize      = 4 * 1024;
    static constexpr size_t kHugePageSize         = 2 * 1024 * 1024;
    static constexpr size_t kPagePoolScale        = 8;
    static constexpr unsigned int kSeed           = 0xC0FFEEU;

    static constexpr size_t kMovingAverageWindow  = 3;
    static constexpr size_t kMinL1CandidatePages  = 64;
    static constexpr double kL1GrowthRatio        = 1.10;
    static constexpr double kL2GrowthRatio        = 1.20;
    static constexpr double kL1MinJumpCycles      = 0.50;
    static constexpr double kL2MinJumpCycles      = 1.00;
    static constexpr size_t kL2SearchGapPoints    = 2;
    static constexpr size_t kSustainPoints        = 1;

    static constexpr bool   kLockMemory           = false;
    static constexpr bool   kDisableThpFor4KPages = true;

    struct Config {
        bool enabled = true;

        size_t max_pages   = kDefaultMaxPages;
        size_t iterations  = kDefaultIterations;

        bool use_huge_pages = false;

        platform::MeasurementEnvironmentOptions environment;
    };

  private:
    struct alignas(64) PageNode {
        PageNode*     next = nullptr;
        std::uint64_t tag  = 0;
    };

    struct Mapping {
        void*  base       = nullptr;
        size_t size_bytes = 0;
        bool   huge_pages = false;
        bool   locked     = false;

        Mapping() = default;
        Mapping(const Mapping&)            = delete;
        Mapping& operator=(const Mapping&) = delete;

        Mapping(Mapping&& other) noexcept
            : base(other.base), size_bytes(other.size_bytes), huge_pages(other.huge_pages), locked(other.locked) {
            other.base       = nullptr;
            other.size_bytes = 0;
            other.huge_pages = false;
            other.locked     = false;
        }

        Mapping& operator=(Mapping&& other) noexcept {
            if (this == &other) {
                return *this;
            }

            release();

            base             = other.base;
            size_bytes       = other.size_bytes;
            huge_pages       = other.huge_pages;
            locked           = other.locked;
            other.base       = nullptr;
            other.size_bytes = 0;
            other.huge_pages = false;
            other.locked     = false;

            return *this;
        }

        ~Mapping() { release(); }

        void release() noexcept {
            if (base == nullptr) {
                return;
            }

            if (locked) {
                munlock(base, size_bytes);
                locked = false;
            }

            if (huge_pages) {
                platform::huge_free(base, size_bytes);
            } else {
                munmap(base, size_bytes);
            }

            base       = nullptr;
            size_bytes = 0;
            huge_pages = false;
        }
    };

    struct DetectionResult {
        std::optional<size_t> l1_index;
        std::optional<size_t> l2_index;
    };

    Config config_;

  public:
    TlbMeasurer() : TlbMeasurer(Config{}) {}

    explicit TlbMeasurer(Config config) : config_(std::move(config)) {
        SPDLOG_INFO("[{}] configured: pages {}..{}, iterations={}, page_size={}, huge_pages={}",
                    name(),
                    kMinPages,
                    config_.max_pages,
                    config_.iterations,
                    page_size_bytes(),
                    config_.use_huge_pages);
    }

    std::string_view name() const noexcept override { return "tlb"; }

    void measure(core::CpuInfoData& data) override {
        validate_config();

        SPDLOG_INFO("[{}] starting TLB benchmark", name());
        platform::ScopedMeasurementEnvironment environment{config_.environment};

        if (!platform::tsc_is_invariant()) {
            SPDLOG_WARN("[{}] invariant TSC is not reported; cycle counts may drift with frequency changes", name());
        }

        const auto   vendor            = platform::arch::detect_vendor();
        const size_t page_size         = page_size_bytes();
        const size_t candidate_pages   = checked_pool_pages(config_.max_pages, kPagePoolScale);
        const size_t allocation_bytes  = checked_total_size(candidate_pages, page_size);
        const size_t cache_line_bytes  = platform::cache_line_size();

        if (!data.cpu_vendor && !vendor.name().empty()) {
            data.cpu_vendor = std::string(vendor.name());
        }

        data.tlb_page_size_bytes = page_size;
        data.tlb_points.clear();
        data.tlb_raw_points.clear();
        data.tlb_l1_size.reset();
        data.tlb_l2_size.reset();

        Mapping                  mapping         = allocate_mapping(allocation_bytes);
        std::vector<size_t>      page_counts     = build_page_counts();
        std::vector<PageNode*>   page_nodes      = page_node_views(mapping.base, candidate_pages, cache_line_bytes);
        std::vector<PageNode*>   candidate_order = page_nodes;
        std::vector<PageNode*>   order(config_.max_pages);
        std::mt19937             rng{kSeed};

        advise_mapping(mapping);
        pretouch_pages(page_nodes);
        warm_instruction_path(page_nodes.front());

        for (const size_t pages : page_counts) {
            const auto point = measure_point(candidate_order, order, rng, pages, page_size);
            data.tlb_points.push_back(point);

            SPDLOG_INFO("[{}] pages={}, bytes={}, median_cpa={:.3f}, min={:.3f}, max={:.3f}",
                        name(),
                        point.pages,
                        point.bytes,
                        point.median_cycles_per_access,
                        point.min_cycles_per_access,
                        point.max_cycles_per_access);
        }

        const auto detection = detect_boundaries(data.tlb_points);
        if (detection.l1_index) {
            data.tlb_l1_size = data.tlb_points[*detection.l1_index].pages;
        }
        if (detection.l2_index) {
            data.tlb_l2_size = data.tlb_points[*detection.l2_index].pages;
        }

        SPDLOG_INFO("[{}] TLB benchmark complete", name());
    }

  private:
    template <typename T>
    static void compiler_barrier(const T& value) {
        asm volatile("" : : "r,m"(value) : "memory");
    }

    size_t page_size_bytes() const noexcept {
        return config_.use_huge_pages ? kHugePageSize : kDefaultPageSize;
    }

    void validate_config() const {
        if (config_.max_pages < kMinPages) {
            throw std::invalid_argument("TLB benchmark max_pages must be >= 1");
        }
        if (config_.iterations == 0) {
            throw std::invalid_argument("TLB benchmark iterations must be positive");
        }
    }

    static size_t checked_total_size(size_t pages, size_t page_size) {
        if (pages > std::numeric_limits<size_t>::max() / page_size) {
            throw std::overflow_error("TLB benchmark allocation size overflows size_t");
        }
        return pages * page_size;
    }

    static size_t checked_pool_pages(size_t pages, size_t scale) {
        if (pages > std::numeric_limits<size_t>::max() / scale) {
            throw std::overflow_error("TLB benchmark page pool size overflows size_t");
        }
        return pages * scale;
    }

    std::vector<size_t> build_page_counts() const {
        std::vector<size_t> counts;

        for (size_t pages = kMinPages; pages <= config_.max_pages;) {
            counts.push_back(pages);

            if (pages == config_.max_pages) {
                break;
            }

            const size_t next_pages = pages > config_.max_pages / kPagesStep
                                        ? config_.max_pages
                                        : pages * kPagesStep;
            if (next_pages <= pages) {
                throw std::overflow_error("TLB benchmark page count progression did not advance");
            }

            pages = next_pages;
        }

        return counts;
    }

    Mapping allocate_mapping(size_t size_bytes) const {
        Mapping mapping{};
        mapping.size_bytes = size_bytes;
        mapping.huge_pages = config_.use_huge_pages;

        if (config_.use_huge_pages) {
            mapping.base = platform::huge_alloc(size_bytes);
            if (mapping.base == nullptr) {
                throw platform::ResourceError("Failed to allocate MAP_HUGETLB memory. Reserve huge pages or disable huge-page mode.");
            }
        } else {
            void* base = mmap(nullptr, size_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (base == MAP_FAILED) {
                throw platform::ResourceError("Failed to allocate benchmark memory: " + std::string(std::strerror(errno)));
            }
            mapping.base = base;
        }

        if constexpr (kLockMemory) {
            if (mlock(mapping.base, mapping.size_bytes) == 0) {
                mapping.locked = true;
            } else {
                SPDLOG_WARN("[{}] mlock failed: {}", name(), std::strerror(errno));
            }
        }

        return mapping;
    }

    void advise_mapping(const Mapping& mapping) const {
        if (mapping.base == nullptr || mapping.huge_pages) {
            return;
        }

#if defined(MADV_NOHUGEPAGE)
        if constexpr (kDisableThpFor4KPages) {
            if (madvise(mapping.base, mapping.size_bytes, MADV_NOHUGEPAGE) != 0) {
                SPDLOG_WARN("[{}] madvise(MADV_NOHUGEPAGE) failed: {}", name(), std::strerror(errno));
            }
        }
#endif
    }

    static size_t page_node_offset(size_t page_index, size_t page_size, size_t cache_line_bytes) {
        const size_t slot_stride    = std::max(cache_line_bytes, sizeof(PageNode));
        const size_t slots_per_page = std::max<size_t>(1, page_size / slot_stride);

        return (page_index % slots_per_page) * slot_stride;
    }

    std::vector<PageNode*> page_node_views(void* base, size_t page_count, size_t cache_line_bytes) const {
        std::vector<PageNode*> pages;
        pages.reserve(page_count);

        auto*        bytes      = static_cast<std::byte*>(base);
        const size_t page_size  = page_size_bytes();

        for (size_t index = 0; index < page_count; ++index) {
            const size_t offset = page_node_offset(index, page_size, cache_line_bytes);
            auto*        node   = reinterpret_cast<PageNode*>(bytes + index * page_size + offset);

            node->next = node;
            node->tag  = index;
            pages.push_back(node);
        }

        return pages;
    }

    void pretouch_pages(const std::vector<PageNode*>& page_nodes) const {
        volatile std::uint64_t checksum = 0;

        for (size_t index = 0; index < page_nodes.size(); ++index) {
            page_nodes[index]->tag = index;
            checksum ^= page_nodes[index]->tag;
        }

        compiler_barrier(checksum);
    }

    static void link_pages(const std::vector<PageNode*>& order, size_t pages) {
        for (size_t index = 0; index + 1 < pages; ++index) {
            order[index]->next = order[index + 1];
        }
        order[pages - 1]->next = order[0];
    }

    void warm_instruction_path(PageNode* start) const {
        warmup(start, 1);
        static_cast<void>(measure_cycles(start));
        platform::arch::mfence();
        platform::arch::lfence();
    }

    __attribute__((noinline)) void warmup(PageNode* start, size_t pages) const {
        const size_t accesses = std::max(pages * kWarmupRounds, pages);
        PageNode*     cursor  = start;

        for (size_t index = 0; index < accesses; ++index) {
            cursor = cursor->next;
        }

        compiler_barrier(cursor);
        platform::arch::mfence();
        platform::arch::lfence();
    }

    __attribute__((noinline)) uint64_t measure_cycles(PageNode* start) const {
        PageNode* cursor = start;
        compiler_barrier(cursor);

        const uint64_t begin = platform::arch::tick();

        size_t remaining = config_.iterations;
        for (; remaining >= 8; remaining -= 8) {
            cursor = cursor->next;
            cursor = cursor->next;
            cursor = cursor->next;
            cursor = cursor->next;
            cursor = cursor->next;
            cursor = cursor->next;
            cursor = cursor->next;
            cursor = cursor->next;
        }
        for (; remaining > 0; --remaining) {
            cursor = cursor->next;
        }

        const uint64_t end = platform::arch::tick();
        compiler_barrier(cursor);

        return end - begin;
    }

    core::TlbSummaryPoint measure_point(std::vector<PageNode*>& candidate_order,
                                        std::vector<PageNode*>& order,
                                        std::mt19937&           rng,
                                        size_t                  pages,
                                        size_t                  page_size) const {
        std::vector<double> samples;
        samples.reserve(kRepeats);

        for (size_t repeat = 0; repeat < kRepeats; ++repeat) {
            std::shuffle(candidate_order.begin(), candidate_order.end(), rng);
            std::copy(candidate_order.begin(), candidate_order.begin() + pages, order.begin());
            link_pages(order, pages);

            PageNode* start = order.front();
            warmup(start, pages);

            const uint64_t cycles            = measure_cycles(start);
            const double   cycles_per_access = static_cast<double>(cycles) / static_cast<double>(config_.iterations);

            samples.push_back(cycles_per_access);
        }

        auto sorted = samples;
        std::sort(sorted.begin(), sorted.end());

        const double min_value    = sorted.front();
        const double max_value    = sorted.back();
        const double median_value = median(sorted);
        const double mean_value   = std::accumulate(samples.begin(), samples.end(), 0.0) / static_cast<double>(samples.size());

        return core::TlbSummaryPoint{
            pages,
            pages * page_size,
            min_value,
            median_value,
            mean_value,
            max_value,
        };
    }

    DetectionResult detect_boundaries(const std::vector<core::TlbSummaryPoint>& points) const {
        DetectionResult result{};
        if (points.size() < 3) {
            return result;
        }

        const auto   smoothed        = moving_average(points);
        const size_t baseline_points = std::min<size_t>(3, points.size());
        double       baseline_sum    = 0.0;

        for (size_t index = 0; index < baseline_points; ++index) {
            baseline_sum += points[index].median_cycles_per_access;
        }

        const double baseline = baseline_sum / static_cast<double>(baseline_points);

        const size_t l1_start_index = std::max<size_t>(1, first_candidate_index(points, kMinL1CandidatePages));
        result.l1_index = find_jump(points, smoothed, l1_start_index, baseline, kL1GrowthRatio, kL1MinJumpCycles);
        if (result.l1_index) {
            const size_t start_index = std::min(points.size(), *result.l1_index + kL2SearchGapPoints);
            result.l2_index = find_jump(points,
                                        smoothed,
                                        start_index,
                                        points[*result.l1_index].median_cycles_per_access,
                                        kL2GrowthRatio,
                                        kL2MinJumpCycles);
        }

        return result;
    }

    static size_t first_candidate_index(const std::vector<core::TlbSummaryPoint>& points, size_t min_pages) {
        for (size_t index = 0; index < points.size(); ++index) {
            if (points[index].pages >= min_pages) {
                return index;
            }
        }

        return points.size();
    }

    static std::vector<double> moving_average(const std::vector<core::TlbSummaryPoint>& points,
                                              size_t width = kMovingAverageWindow) {
        const size_t safe_width = std::max<size_t>(1, width);
        std::vector<double> smoothed(points.size(), 0.0);

        for (size_t index = 0; index < points.size(); ++index) {
            const size_t left = index + 1 > safe_width ? index + 1 - safe_width : 0;
            double       sum  = 0.0;

            for (size_t sample = left; sample <= index; ++sample) {
                sum += points[sample].median_cycles_per_access;
            }

            smoothed[index] = sum / static_cast<double>(index - left + 1);
        }

        return smoothed;
    }

    std::optional<size_t> find_jump(const std::vector<core::TlbSummaryPoint>& points,
                                    const std::vector<double>&                smoothed,
                                    size_t                                    start_index,
                                    double                                    reference_level,
                                    double                                    ratio_threshold,
                                    double                                    min_jump_cycles) const {
        if (start_index >= points.size()) {
            return std::nullopt;
        }

        const double safe_reference = std::max(reference_level, 0.001);

        for (size_t index = start_index; index < points.size(); ++index) {
            const double current         = points[index].median_cycles_per_access;
            const double previous        = points[index - 1].median_cycles_per_access;
            const double jump            = current - previous;
            const double local_ratio     = previous > 0.0 ? current / previous : 0.0;
            const double reference_ratio = current / safe_reference;

            if (jump >= min_jump_cycles &&
                local_ratio >= ratio_threshold &&
                reference_ratio >= ratio_threshold &&
                is_sustained(smoothed, index)) {
                return index;
            }
        }

        return std::nullopt;
    }

    static bool is_sustained(const std::vector<double>& smoothed, size_t index) {
        if (kSustainPoints == 0 || index + kSustainPoints >= smoothed.size()) {
            return true;
        }

        const double floor = smoothed[index] * 0.97;
        for (size_t offset = 1; offset <= kSustainPoints; ++offset) {
            if (smoothed[index + offset] < floor) {
                return false;
            }
        }

        return true;
    }

    static double median(const std::vector<double>& sorted_values) {
        const size_t size = sorted_values.size();
        if ((size % 2U) == 0U) {
            return (sorted_values[size / 2U - 1U] + sorted_values[size / 2U]) * 0.5;
        }
        return sorted_values[size / 2U];
    }
};

} // namespace silicon_probe::tlb
