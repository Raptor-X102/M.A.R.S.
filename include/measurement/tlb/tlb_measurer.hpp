#pragma once

#include "infra/logging.hpp"
#include "measurement/core/measurer.hpp"
#include "platform/arch.hpp"
#include "platform/os.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
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

enum class GrowthMode {
    Multiply = 0,
    Add = 1,
};

class TlbMeasurer final : public core::Measurer {
  public:
    static constexpr size_t kDefaultMinPages = 1;
    static constexpr size_t kDefaultMaxPages = 4096;
    static constexpr size_t kDefaultPagesStep = 2;
    static constexpr size_t kDefaultIterations = 2'000'000;
    static constexpr size_t kDefaultRepeats = 7;
    static constexpr size_t kDefaultWarmupRounds = 2;
    static constexpr size_t kDefaultPageSize = 4 * 1024;
    static constexpr size_t kHugePageSize = 2 * 1024 * 1024;
    static constexpr unsigned int kDefaultSeed = 0xC0FFEEU;

    struct DetectionConfig {
        size_t moving_average_window = 3;
        double l1_growth_ratio = 1.10;
        double l2_growth_ratio = 1.08;
        double page_walk_growth_ratio = 1.12;
        double min_jump_cycles = 0.75;
        double page_walk_jump_cycles = 2.0;
        size_t sustain_points = 1;
    };

    struct Config {
        bool enabled = true;
        size_t min_pages = kDefaultMinPages;
        size_t max_pages = kDefaultMaxPages;
        size_t pages_step = kDefaultPagesStep;
        GrowthMode growth_mode = GrowthMode::Multiply;
        size_t iterations = kDefaultIterations;
        size_t repeats = kDefaultRepeats;
        size_t warmup_rounds = kDefaultWarmupRounds;
        size_t page_size_bytes = kDefaultPageSize;
        bool use_huge_pages = false;
        bool disable_transparent_huge_pages = true;
        bool lock_memory = false;
        unsigned int seed = kDefaultSeed;
        DetectionConfig detection;
        platform::MeasurementEnvironmentOptions environment;
    };

  private:
    struct alignas(64) PageNode {
        PageNode* next = nullptr;
        std::uint64_t tag = 0;
    };

    struct Mapping {
        void* base = nullptr;
        size_t size_bytes = 0;
        bool huge_pages = false;
        bool locked = false;

        Mapping() = default;
        Mapping(const Mapping&) = delete;
        Mapping& operator=(const Mapping&) = delete;

        Mapping(Mapping&& other) noexcept
            : base(other.base), size_bytes(other.size_bytes), huge_pages(other.huge_pages), locked(other.locked) {
            other.base = nullptr;
            other.size_bytes = 0;
            other.huge_pages = false;
            other.locked = false;
        }

        Mapping& operator=(Mapping&& other) noexcept {
            if (this == &other) {
                return *this;
            }

            release();
            base = other.base;
            size_bytes = other.size_bytes;
            huge_pages = other.huge_pages;
            locked = other.locked;
            other.base = nullptr;
            other.size_bytes = 0;
            other.huge_pages = false;
            other.locked = false;
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

            base = nullptr;
            size_bytes = 0;
            huge_pages = false;
        }
    };

    struct DetectionResult {
        std::optional<size_t> l1_index;
        std::optional<size_t> l2_index;
        std::optional<size_t> page_walk_index;
    };

    Config config_;

  public:
    TlbMeasurer() : TlbMeasurer(Config{}) {}

    explicit TlbMeasurer(Config config) : config_(std::move(config)) {
        SPDLOG_INFO("[{}] configured: pages {}..{}, step={}, mode={}, iterations={}, repeats={}, page_size={}, huge_pages={}",
                    name(), config_.min_pages, config_.max_pages, config_.pages_step,
                    config_.growth_mode == GrowthMode::Multiply ? "multiply" : "add", config_.iterations, config_.repeats,
                    config_.page_size_bytes, config_.use_huge_pages);
    }

    std::string_view name() const noexcept override { return "tlb"; }

    void measure(core::CpuInfoData& data) override {
        validate_config();

        SPDLOG_INFO("[{}] starting TLB benchmark", name());
        platform::ScopedMeasurementEnvironment environment{config_.environment};

        if (!platform::tsc_is_invariant()) {
            SPDLOG_WARN("[{}] invariant TSC is not reported; cycle counts may drift with frequency changes", name());
        }

        const auto vendor = platform::arch::detect_vendor();
        if (!data.cpu_vendor && !vendor.name().empty()) {
            data.cpu_vendor = std::string(vendor.name());
        }

        data.tlb_page_size_bytes = config_.page_size_bytes;
        data.tlb_points.clear();
        data.tlb_raw_points.clear();
        data.tlb_l1_size.reset();
        data.tlb_l2_size.reset();
        data.tlb_page_walk_threshold.reset();

        const std::vector<size_t> page_counts = build_page_counts();
        const size_t total_bytes = checked_total_size(config_.max_pages, config_.page_size_bytes);
        Mapping mapping = allocate_mapping(total_bytes);
        advise_mapping(mapping);

        std::vector<PageNode*> page_nodes = page_node_views(mapping.base, config_.max_pages);
        pretouch_pages(page_nodes);

        std::vector<PageNode*> order(config_.max_pages);
        std::mt19937 rng(config_.seed);

        for (const size_t pages : page_counts) {
            std::copy(page_nodes.begin(), page_nodes.begin() + pages, order.begin());

            std::vector<double> samples;
            samples.reserve(config_.repeats);

            for (size_t repeat = 0; repeat < config_.repeats; ++repeat) {
                std::shuffle(order.begin(), order.begin() + pages, rng);
                link_pages(order, pages);

                PageNode* start = order.front();
                warmup(start, pages);

                const uint64_t cycles = measure_cycles(start);
                const double cycles_per_access = static_cast<double>(cycles) / static_cast<double>(config_.iterations);

                samples.push_back(cycles_per_access);
                data.tlb_raw_points.push_back(core::TlbRawPoint{
                    pages,
                    pages * config_.page_size_bytes,
                    repeat,
                    cycles_per_access,
                });
            }

            auto sorted = samples;
            std::sort(sorted.begin(), sorted.end());

            const double min_value = sorted.front();
            const double max_value = sorted.back();
            const double median_value = median(sorted);
            const double mean_value = std::accumulate(samples.begin(), samples.end(), 0.0) / static_cast<double>(samples.size());

            data.tlb_points.push_back(core::TlbSummaryPoint{
                pages,
                pages * config_.page_size_bytes,
                min_value,
                median_value,
                mean_value,
                max_value,
            });

            SPDLOG_INFO("[{}] pages={}, bytes={}, median_cpa={:.3f}, min={:.3f}, max={:.3f}", name(), pages,
                        pages * config_.page_size_bytes, median_value, min_value, max_value);
        }

        const DetectionResult detection = detect_boundaries(data.tlb_points);
        if (detection.l1_index) {
            data.tlb_l1_size = data.tlb_points[*detection.l1_index].pages;
        }
        if (detection.l2_index) {
            data.tlb_l2_size = data.tlb_points[*detection.l2_index].pages;
        }
        if (detection.page_walk_index) {
            data.tlb_page_walk_threshold = data.tlb_points[*detection.page_walk_index].pages;
        }

        SPDLOG_INFO("[{}] TLB benchmark complete", name());
    }

  private:
    template <typename T> static void compiler_barrier(const T& value) {
        asm volatile("" : : "r,m"(value) : "memory");
    }

    void validate_config() const {
        if (config_.min_pages == 0) {
            throw std::invalid_argument("TLB benchmark min_pages must be positive");
        }
        if (config_.max_pages < config_.min_pages) {
            throw std::invalid_argument("TLB benchmark max_pages must be >= min_pages");
        }
        if (config_.iterations == 0) {
            throw std::invalid_argument("TLB benchmark iterations must be positive");
        }
        if (config_.repeats == 0) {
            throw std::invalid_argument("TLB benchmark repeats must be positive");
        }
        if (config_.page_size_bytes < sizeof(PageNode)) {
            throw std::invalid_argument("TLB benchmark page_size_bytes must fit the pointer-chasing node");
        }
        if (config_.use_huge_pages && config_.page_size_bytes != kHugePageSize) {
            throw std::invalid_argument("TLB huge-pages mode currently requires page_size_bytes = 2 MiB");
        }
        if (config_.growth_mode == GrowthMode::Multiply && config_.pages_step <= 1) {
            throw std::invalid_argument("TLB benchmark pages_step must be > 1 in multiply mode");
        }
        if (config_.growth_mode == GrowthMode::Add && config_.pages_step == 0) {
            throw std::invalid_argument("TLB benchmark pages_step must be positive in add mode");
        }
    }

    static size_t checked_total_size(size_t pages, size_t page_size_bytes) {
        if (pages > std::numeric_limits<size_t>::max() / page_size_bytes) {
            throw std::overflow_error("TLB benchmark allocation size overflows size_t");
        }
        return pages * page_size_bytes;
    }

    std::vector<size_t> build_page_counts() const {
        std::vector<size_t> counts;

        for (size_t pages = config_.min_pages; pages <= config_.max_pages;) {
            counts.push_back(pages);

            if (pages == config_.max_pages) {
                break;
            }

            size_t next_pages = pages;
            if (config_.growth_mode == GrowthMode::Multiply) {
                if (pages > config_.max_pages / config_.pages_step) {
                    next_pages = config_.max_pages;
                } else {
                    next_pages = pages * config_.pages_step;
                }
            } else {
                if (config_.pages_step >= config_.max_pages || pages > config_.max_pages - config_.pages_step) {
                    next_pages = config_.max_pages;
                } else {
                    next_pages = pages + config_.pages_step;
                }
            }

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

        if (config_.use_huge_pages) {
            mapping.base = platform::huge_alloc(size_bytes);
            mapping.huge_pages = true;
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

        if (config_.lock_memory) {
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
        if (config_.disable_transparent_huge_pages) {
            if (madvise(mapping.base, mapping.size_bytes, MADV_NOHUGEPAGE) != 0) {
                SPDLOG_WARN("[{}] madvise(MADV_NOHUGEPAGE) failed: {}", name(), std::strerror(errno));
            }
        }
#endif
    }

    std::vector<PageNode*> page_node_views(void* base, size_t page_count) const {
        std::vector<PageNode*> pages;
        pages.reserve(page_count);

        auto* bytes = static_cast<std::byte*>(base);
        for (size_t index = 0; index < page_count; ++index) {
            auto* node = reinterpret_cast<PageNode*>(bytes + index * config_.page_size_bytes);
            node->next = node;
            node->tag = index;
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

    void warmup(PageNode* start, size_t pages) const {
        const size_t accesses = std::max(pages * config_.warmup_rounds, pages);
        PageNode* cursor = start;

        for (size_t index = 0; index < accesses; ++index) {
            cursor = cursor->next;
        }

        compiler_barrier(cursor);
        platform::arch::mfence();
        platform::arch::lfence();
    }

    uint64_t measure_cycles(PageNode* start) const {
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

    DetectionResult detect_boundaries(const std::vector<core::TlbSummaryPoint>& points) const {
        DetectionResult result{};
        if (points.size() < 3) {
            return result;
        }

        const std::vector<double> smoothed = moving_average(points);
        const size_t baseline_points = std::min<size_t>(3, smoothed.size());
        const double baseline =
            std::accumulate(smoothed.begin(), smoothed.begin() + static_cast<std::ptrdiff_t>(baseline_points), 0.0) /
            static_cast<double>(baseline_points);

        result.l1_index = find_jump(smoothed, 1, baseline, config_.detection.l1_growth_ratio, config_.detection.min_jump_cycles);
        if (result.l1_index) {
            result.l2_index = find_jump(smoothed, *result.l1_index + 1, smoothed[*result.l1_index], config_.detection.l2_growth_ratio,
                                        config_.detection.min_jump_cycles);
        }
        if (result.l2_index) {
            result.page_walk_index =
                find_jump(smoothed, *result.l2_index + 1, smoothed[*result.l2_index], config_.detection.page_walk_growth_ratio,
                          config_.detection.page_walk_jump_cycles);
        }

        return result;
    }

    std::vector<double> moving_average(const std::vector<core::TlbSummaryPoint>& points) const {
        const size_t width = std::max<size_t>(1, config_.detection.moving_average_window);
        const size_t radius = width / 2;

        std::vector<double> smoothed(points.size(), 0.0);
        for (size_t index = 0; index < points.size(); ++index) {
            const size_t left = index > radius ? index - radius : 0;
            const size_t right = std::min(points.size() - 1, index + radius);

            double sum = 0.0;
            for (size_t sample = left; sample <= right; ++sample) {
                sum += points[sample].median_cycles_per_access;
            }

            smoothed[index] = sum / static_cast<double>(right - left + 1);
        }

        return smoothed;
    }

    std::optional<size_t> find_jump(const std::vector<double>& smoothed, size_t start_index, double reference_level, double ratio_threshold,
                                    double min_jump_cycles) const {
        if (start_index >= smoothed.size()) {
            return std::nullopt;
        }

        const double safe_reference = std::max(reference_level, 0.001);

        for (size_t index = start_index; index < smoothed.size(); ++index) {
            const double previous = smoothed[index - 1];
            const double jump = smoothed[index] - previous;
            const double local_ratio = previous > 0.0 ? smoothed[index] / previous : 0.0;
            const double reference_ratio = smoothed[index] / safe_reference;

            if (jump >= min_jump_cycles && local_ratio >= ratio_threshold && reference_ratio >= ratio_threshold &&
                is_sustained(smoothed, index)) {
                return index;
            }
        }

        return std::nullopt;
    }

    bool is_sustained(const std::vector<double>& smoothed, size_t index) const {
        const size_t sustain_points = config_.detection.sustain_points;
        if (sustain_points == 0 || index + sustain_points >= smoothed.size()) {
            return true;
        }

        const double floor = smoothed[index] * 0.97;
        for (size_t offset = 1; offset <= sustain_points; ++offset) {
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
