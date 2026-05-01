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

/**
 * @brief Measures TLB behavior of the current CPU.
 *
 * This class measures the cost of memory accesses when the program touches
 * different numbers of memory pages.
 *
 * TLB means Translation Lookaside Buffer. It is a small CPU cache for virtual
 * to physical address translations.
 *
 * The benchmark uses pointer chasing. Each node is placed on a separate memory
 * page. The code follows pointers from one page to another. This makes memory
 * accesses dependent, so the CPU cannot easily hide the latency.
 *
 * The result can be used to estimate L1 and L2 TLB sizes.
 */
class TlbMeasurer final : public core::Measurer {
   public:
    /**
     * @brief Minimum number of pages used in the benchmark.
     */
    static constexpr size_t kMinPages = 1;

    /**
     * @brief Default maximum number of pages used in the benchmark.
     */
    static constexpr size_t kDefaultMaxPages = 4096;

    /**
     * @brief Page count multiplier between benchmark points.
     *
     * With value 2, the benchmark uses page counts like:
     * 1, 2, 4, 8, 16, 32, and so on.
     */
    static constexpr size_t kPagesStep = 2;

    /**
     * @brief Default number of pointer accesses for one measurement.
     *
     * A higher value gives more stable results, but the benchmark takes longer.
     */
    static constexpr size_t kDefaultIterations = 2'000'000;

    /**
     * @brief Number of repeated measurements for one page count.
     *
     * The benchmark uses several samples and then takes the median.
     */
    static constexpr size_t kRepeats = 9;

    /**
     * @brief Number of warmup passes before real measurement.
     */
    static constexpr size_t kWarmupRounds = 2;

    /**
     * @brief Size of a normal memory page.
     */
    static constexpr size_t kDefaultPageSize = 4 * 1024;

    /**
     * @brief Size of a huge memory page.
     */
    static constexpr size_t kHugePageSize = 2 * 1024 * 1024;

    /**
     * @brief Multiplier for the internal page pool.
     *
     * The benchmark allocates more pages than it uses in one test point.
     * This allows it to choose random pages from a larger pool.
     */
    static constexpr size_t kPagePoolScale = 8;

    /**
     * @brief Fixed seed for random page order.
     *
     * A fixed seed makes benchmark runs easier to compare.
     */
    static constexpr unsigned int kSeed = 0xC0FFEEU;

    /**
     * @brief The first page count where L1 TLB detection may start.
     *
     * This helps to avoid false jumps on very small page counts.
     */
    static constexpr size_t kMinL1CandidatePages = 64;

    /**
     * @brief Minimum relative latency growth for L1 TLB detection.
     */
    static constexpr double kL1GrowthRatio = 1.10;

    /**
     * @brief Minimum relative latency growth for L2 TLB detection.
     */
    static constexpr double kL2GrowthRatio = 1.20;

    /**
     * @brief Minimum absolute latency jump for L1 TLB detection.
     *
     * The value is measured in cycles per access.
     */
    static constexpr double kL1MinJumpCycles = 0.50;

    /**
     * @brief Minimum absolute latency jump for L2 TLB detection.
     *
     * The value is measured in cycles per access.
     */
    static constexpr double kL2MinJumpCycles = 1.00;

    /**
     * @brief Number of points skipped after L1 before searching for L2.
     *
     * This helps not to detect the same jump twice.
     */
    static constexpr size_t kL2SearchGapPoints = 2;

    /**
     * @brief Enables memory locking with mlock().
     *
     * When this value is true, the benchmark tries to keep memory out of swap.
     * It is false by default because mlock() may need special system limits.
     */
    static constexpr bool kLockMemory = false;

    /**
     * @brief Disables transparent huge pages for normal 4 KB pages.
     *
     * This is useful because transparent huge pages can change the result of
     * a 4 KB page TLB benchmark.
     */
    static constexpr bool kDisableThpFor4KPages = true;

    /**
     * @brief Configuration for the TLB benchmark.
     */
    struct Config {
        /**
         * @brief Enables or disables this benchmark.
         *
         * This field is not checked inside this class. It can be used by a
         * higher-level benchmark manager.
         */
        bool enabled = true;

        /**
         * @brief Maximum number of pages to test.
         */
        size_t max_pages = kDefaultMaxPages;

        /**
         * @brief Number of pointer accesses in one measurement.
         */
        size_t iterations = kDefaultIterations;

        /**
         * @brief Use huge pages instead of normal pages.
         *
         * If false, the benchmark uses 4 KB pages.
         * If true, the benchmark uses 2 MB huge pages.
         */
        bool use_huge_pages = false;

        /**
         * @brief Options for the measurement environment.
         *
         * These options may control CPU pinning, priority, or other settings.
         */
        platform::MeasurementEnvironmentOptions environment;
    };

   private:
    /**
     * @brief One node in the pointer chasing chain.
     *
     * Each node is placed on a separate memory page.
     */
    struct alignas(64) PageNode {
        /**
         * @brief Pointer to the next node.
         */
        PageNode* next = nullptr;

        /**
         * @brief Small value used to touch the page.
         *
         * It also helps to stop the compiler from removing memory accesses.
         */
        std::uint64_t tag = 0;
    };

    /**
     * @brief RAII wrapper for benchmark memory.
     *
     * The object owns a memory mapping. It frees the memory in the destructor.
     */
    struct Mapping {
        /**
         * @brief Start address of the memory block.
         */
        void* base = nullptr;

        /**
         * @brief Size of the memory block in bytes.
         */
        size_t size_bytes = 0;

        /**
         * @brief True if memory was allocated as huge pages.
         */
        bool huge = false;

        /**
         * @brief True if memory was locked with mlock().
         */
        bool locked = false;

        Mapping() = default;

        Mapping(const Mapping&) = delete;
        Mapping& operator=(const Mapping&) = delete;

        /**
         * @brief Move constructor.
         *
         * Ownership of memory is moved from another object.
         *
         * @param other Source mapping.
         */
        Mapping(Mapping&& other) noexcept { *this = std::move(other); }

        /**
         * @brief Move assignment operator.
         *
         * Old memory is released. Then ownership is moved from another object.
         *
         * @param other Source mapping.
         * @return Reference to this object.
         */
        Mapping& operator=(Mapping&& other) noexcept {
            if (this != &other) {
                release();

                base = std::exchange(other.base, nullptr);
                size_bytes = std::exchange(other.size_bytes, 0);
                huge = std::exchange(other.huge, false);
                locked = std::exchange(other.locked, false);
            }

            return *this;
        }

        /**
         * @brief Releases owned memory.
         */
        ~Mapping() { release(); }

        /**
         * @brief Frees the memory block.
         *
         * If the memory was locked, it is unlocked first.
         * Huge pages and normal pages are freed in different ways.
         */
        void release() noexcept {
            if (base == nullptr) {
                return;
            }

            if (locked) {
                munlock(base, size_bytes);
            }

            if (huge) {
                platform::huge_free(base, size_bytes);
            } else {
                munmap(base, size_bytes);
            }

            base = nullptr;
            size_bytes = 0;
            huge = false;
            locked = false;
        }
    };

    /**
     * @brief Detected TLB boundary indexes.
     *
     * The values are indexes inside the measured points array.
     */
    struct Boundaries {
        /**
         * @brief Index of the possible L1 TLB boundary.
         */
        std::optional<size_t> l1;

        /**
         * @brief Index of the possible L2 TLB boundary.
         */
        std::optional<size_t> l2;
    };

    /**
     * @brief Current benchmark configuration.
     */
    Config config_;

   public:
    /**
     * @brief Creates a TLB measurer with default configuration.
     */
    TlbMeasurer() : TlbMeasurer(Config{}) {}

    /**
     * @brief Creates a TLB measurer with user configuration.
     *
     * @param config Benchmark configuration.
     */
    explicit TlbMeasurer(Config config) : config_(std::move(config)) {
        SPDLOG_INFO("[{}] configured: pages {}..{}, iterations={}, page_size={}, huge_pages={}", name(), kMinPages,
                    config_.max_pages, config_.iterations, page_size_bytes(), config_.use_huge_pages);
    }

    /**
     * @brief Returns the name of this measurer.
     *
     * @return Name of the measurer.
     */
    std::string_view name() const noexcept override { return "tlb"; }

    /**
     * @brief Runs the TLB benchmark and stores the result.
     *
     * This function allocates memory, builds page chains, measures access
     * latency for different page counts, and tries to detect L1 and L2 TLB
     * boundaries.
     *
     * @param data Output CPU information structure.
     *
     * @throws std::invalid_argument If configuration is invalid.
     * @throws std::overflow_error If calculated memory size is too large.
     * @throws platform::ResourceError If memory allocation fails.
     */
    void measure(shared_types::CpuInfoData& data) override {
        validate_config();

        SPDLOG_INFO("[{}] starting TLB benchmark", name());

        platform::ScopedMeasurementEnvironment environment{config_.environment};

        if (!platform::tsc_is_invariant()) {
            SPDLOG_WARN("[{}] invariant TSC is not reported; cycle counts may drift", name());
        }

        const auto vendor = platform::arch::detect_vendor();
        if (!data.cpu_vendor && !vendor.name().empty()) {
            data.cpu_vendor = vendor;
        }

        const size_t page_size = page_size_bytes();
        const size_t pool_pages = checked_mul(config_.max_pages, kPagePoolScale);
        const size_t allocation_bytes = checked_mul(pool_pages, page_size);
        const size_t cache_line = platform::cache_line_size();

        data.tlb_page_size_bytes = page_size;
        data.tlb_l1_size.reset();
        data.tlb_l2_size.reset();

        Mapping mapping = allocate_mapping(allocation_bytes);
        advise_mapping(mapping);

        std::vector<size_t> page_counts = build_page_counts();
        std::vector<PageNode*> pool = make_page_nodes(mapping.base, pool_pages, page_size, cache_line);
        std::vector<PageNode*> order(config_.max_pages);
        std::vector<shared_types::TlbSummaryPoint> points;
        points.reserve(page_counts.size());

        pretouch(pool);
        warm_instruction_path(pool.front());

        std::mt19937 rng{kSeed};

        for (size_t pages : page_counts) {
            shared_types::TlbSummaryPoint point = measure_point(pool, order, rng, pages, page_size);
            points.push_back(point);

            SPDLOG_INFO("[{}] pages={}, bytes={}, median_cpa={:.3f}, min={:.3f}, max={:.3f}", name(), point.pages,
                        point.bytes, point.median_cycles_per_access, point.min_cycles_per_access,
                        point.max_cycles_per_access);
        }

        const Boundaries boundaries = detect_boundaries(points);

        if (boundaries.l1) {
            data.tlb_l1_size = points[*boundaries.l1].pages;
        }

        if (boundaries.l2) {
            data.tlb_l2_size = points[*boundaries.l2].pages;
        }

        SPDLOG_INFO("[{}] TLB benchmark complete", name());
    }

   private:
    /**
     * @brief Stops the compiler from removing or moving important operations.
     *
     * This is a compiler barrier, not a CPU barrier.
     *
     * @tparam T Type of the protected value.
     * @param value Value that must look used to the compiler.
     */
    template <typename T>
    static void compiler_barrier(const T& value) {
        asm volatile("" : : "r,m"(value) : "memory");
    }

    /**
     * @brief Returns the page size used by this benchmark.
     *
     * @return 4 KB for normal pages, or 2 MB for huge pages.
     */
    size_t page_size_bytes() const noexcept { return config_.use_huge_pages ? kHugePageSize : kDefaultPageSize; }

    /**
     * @brief Checks that the configuration is valid.
     *
     * @throws std::invalid_argument If max_pages is less than 1.
     * @throws std::invalid_argument If iterations is zero.
     */
    void validate_config() const {
        if (config_.max_pages < kMinPages) {
            throw std::invalid_argument("TLB benchmark max_pages must be >= 1");
        }

        if (config_.iterations == 0) {
            throw std::invalid_argument("TLB benchmark iterations must be positive");
        }
    }

    /**
     * @brief Multiplies two size_t values and checks overflow.
     *
     * @param lhs Left value.
     * @param rhs Right value.
     * @return Result of lhs * rhs.
     *
     * @throws std::overflow_error If multiplication overflows.
     */
    static size_t checked_mul(size_t lhs, size_t rhs) {
        if (rhs != 0 && lhs > std::numeric_limits<size_t>::max() / rhs) {
            throw std::overflow_error("TLB benchmark size overflows size_t");
        }

        return lhs * rhs;
    }

    /**
     * @brief Builds the list of page counts for benchmark points.
     *
     * The result is usually powers of two. The final max_pages value is also
     * added if it is not already in the list.
     *
     * @return Vector with page counts.
     */
    std::vector<size_t> build_page_counts() const {
        std::vector<size_t> result;

        for (size_t pages = kMinPages; pages <= config_.max_pages; pages *= kPagesStep) {
            result.push_back(pages);

            if (pages > config_.max_pages / kPagesStep) {
                break;
            }
        }

        if (result.back() != config_.max_pages) {
            result.push_back(config_.max_pages);
        }

        return result;
    }

    /**
     * @brief Allocates memory for the benchmark.
     *
     * Normal pages are allocated with mmap().
     * Huge pages are allocated with platform::huge_alloc().
     *
     * @param size_bytes Number of bytes to allocate.
     * @return RAII memory mapping object.
     *
     * @throws platform::ResourceError If allocation fails.
     */
    Mapping allocate_mapping(size_t size_bytes) const {
        Mapping mapping;
        mapping.size_bytes = size_bytes;
        mapping.huge = config_.use_huge_pages;

        if (config_.use_huge_pages) {
            mapping.base = platform::huge_alloc(size_bytes);

            if (mapping.base == nullptr) {
                throw platform::ResourceError(
                    "Failed to allocate MAP_HUGETLB memory. Reserve huge pages or disable huge-page mode.");
            }
        } else {
            mapping.base = mmap(nullptr, size_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

            if (mapping.base == MAP_FAILED) {
                mapping.base = nullptr;
                throw platform::ResourceError("Failed to allocate benchmark memory: " +
                                              std::string(std::strerror(errno)));
            }
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

    /**
     * @brief Gives memory advice to the operating system.
     *
     * For normal pages, this function tries to disable transparent huge pages.
     * For huge pages, it does nothing.
     *
     * @param mapping Memory mapping.
     */
    void advise_mapping(const Mapping& mapping) const {
        if (mapping.base == nullptr || mapping.huge) {
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

    /**
     * @brief Creates PageNode pointers inside the memory block.
     *
     * Each page gets one PageNode. The node offset changes between pages.
     * This makes the access pattern less regular.
     *
     * @param base Start of the memory block.
     * @param page_count Number of pages in the block.
     * @param page_size Size of one page.
     * @param cache_line_bytes Size of one cache line.
     * @return Vector with pointers to page nodes.
     */
    static std::vector<PageNode*> make_page_nodes(void* base, size_t page_count, size_t page_size,
                                                  size_t cache_line_bytes) {
        std::vector<PageNode*> nodes;
        nodes.reserve(page_count);

        auto* bytes = static_cast<std::byte*>(base);

        const size_t stride = std::max(cache_line_bytes, sizeof(PageNode));
        const size_t slots_per_page = std::max<size_t>(1, page_size / stride);

        for (size_t page = 0; page < page_count; ++page) {
            const size_t offset = (page % slots_per_page) * stride;
            auto* node = reinterpret_cast<PageNode*>(bytes + page * page_size + offset);

            node->next = node;
            node->tag = page;

            nodes.push_back(node);
        }

        return nodes;
    }

    /**
     * @brief Touches all pages before the real benchmark.
     *
     * This makes the operating system allocate physical pages before the timed
     * code starts.
     *
     * @param nodes Page nodes to touch.
     */
    static void pretouch(const std::vector<PageNode*>& nodes) {
        volatile std::uint64_t checksum = 0;

        for (size_t i = 0; i < nodes.size(); ++i) {
            nodes[i]->tag = i;
            checksum ^= nodes[i]->tag;
        }

        compiler_barrier(checksum);
    }

    /**
     * @brief Links selected pages into a circular pointer chain.
     *
     * Example:
     * A -> B -> C -> A
     *
     * @param order Selected page nodes.
     * @param count Number of selected nodes.
     */
    static void link_ring(const std::vector<PageNode*>& order, size_t count) {
        for (size_t i = 0; i + 1 < count; ++i) {
            order[i]->next = order[i + 1];
        }

        order[count - 1]->next = order[0];
    }

    /**
     * @brief Warms up the instruction path.
     *
     * This runs the measurement code once before real measurements.
     *
     * @param start First node in the chain.
     */
    void warm_instruction_path(PageNode* start) const {
        warmup(start, 1);
        static_cast<void>(measure_cycles(start));

        platform::arch::mfence();
        platform::arch::lfence();
    }

    /**
     * @brief Runs a short pointer chasing loop before measurement.
     *
     * Warmup helps to reduce first-run effects.
     *
     * @param start First node in the chain.
     * @param pages Number of pages in the chain.
     */
    __attribute__((noinline)) void warmup(PageNode* start, size_t pages) const {
        PageNode* cursor = start;
        size_t accesses = std::max(pages, pages * kWarmupRounds);

        for (size_t i = 0; i < accesses; ++i) {
            cursor = cursor->next;
        }

        compiler_barrier(cursor);

        platform::arch::mfence();
        platform::arch::lfence();
    }

    /**
     * @brief Measures CPU cycles for pointer chasing.
     *
     * The function follows the pointer chain config_.iterations times.
     * The loop is unrolled by 8 to reduce loop overhead.
     *
     * @param start First node in the chain.
     * @return Number of CPU cycles used by all accesses.
     */
    __attribute__((noinline)) std::uint64_t measure_cycles(PageNode* start) const {
        PageNode* cursor = start;
        compiler_barrier(cursor);

        const std::uint64_t begin = platform::arch::tick();

        size_t remaining = config_.iterations;

        while (remaining >= 8) {
            cursor = cursor->next;
            cursor = cursor->next;
            cursor = cursor->next;
            cursor = cursor->next;
            cursor = cursor->next;
            cursor = cursor->next;
            cursor = cursor->next;
            cursor = cursor->next;

            remaining -= 8;
        }

        while (remaining-- > 0) {
            cursor = cursor->next;
        }

        const std::uint64_t end = platform::arch::tick();

        compiler_barrier(cursor);

        return end - begin;
    }

    /**
     * @brief Measures one benchmark point.
     *
     * One point means one page count, for example 64 pages or 128 pages.
     *
     * The function repeats the measurement several times and returns min,
     * median, mean, and max cycles per access.
     *
     * @param pool Full pool of available page nodes.
     * @param order Temporary vector for selected page nodes.
     * @param rng Random number generator.
     * @param pages Number of pages for this point.
     * @param page_size Size of one page.
     * @return Summary point for this page count.
     */
    shared_types::TlbSummaryPoint measure_point(std::vector<PageNode*>& pool, std::vector<PageNode*>& order,
                                                std::mt19937& rng, size_t pages, size_t page_size) const {
        std::vector<double> samples;
        samples.reserve(kRepeats);

        for (size_t repeat = 0; repeat < kRepeats; ++repeat) {
            std::shuffle(pool.begin(), pool.end(), rng);
            std::copy_n(pool.begin(), pages, order.begin());

            link_ring(order, pages);

            PageNode* start = order.front();

            warmup(start, pages);

            const std::uint64_t cycles = measure_cycles(start);
            samples.push_back(static_cast<double>(cycles) / static_cast<double>(config_.iterations));
        }

        std::vector<double> sorted = samples;
        std::sort(sorted.begin(), sorted.end());

        const double min_value = sorted.front();
        const double max_value = sorted.back();
        const double median_value = median(sorted);
        const double mean_value =
            std::accumulate(samples.begin(), samples.end(), 0.0) / static_cast<double>(samples.size());

        return shared_types::TlbSummaryPoint{
            pages, pages * page_size, min_value, median_value, mean_value, max_value,
        };
    }

    /**
     * @brief Detects possible L1 and L2 TLB boundaries.
     *
     * A boundary is detected as a visible latency jump.
     *
     * @param points Measured benchmark points.
     * @return Detected boundary indexes.
     */
    Boundaries detect_boundaries(const std::vector<shared_types::TlbSummaryPoint>& points) const {
        Boundaries result;

        if (points.size() < 3) {
            return result;
        }

        const double baseline = mean_first_points(points, 3);

        const size_t l1_start = first_index_with_at_least_pages(points, kMinL1CandidatePages);

        result.l1 = find_latency_jump(points, l1_start, baseline, kL1GrowthRatio, kL1MinJumpCycles);

        if (result.l1) {
            const size_t l2_start = *result.l1 + kL2SearchGapPoints;
            const double l1_level = points[*result.l1].median_cycles_per_access;

            result.l2 = find_latency_jump(points, l2_start, l1_level, kL2GrowthRatio, kL2MinJumpCycles);
        }

        return result;
    }

    /**
     * @brief Calculates the mean latency of the first measured points.
     *
     * This value is used as a baseline.
     *
     * @param points Measured benchmark points.
     * @param max_count Maximum number of first points to use.
     * @return Mean median latency.
     */
    static double mean_first_points(const std::vector<shared_types::TlbSummaryPoint>& points, size_t max_count) {
        const size_t count = std::min(points.size(), max_count);

        double sum = 0.0;

        for (size_t i = 0; i < count; ++i) {
            sum += points[i].median_cycles_per_access;
        }

        return sum / static_cast<double>(count);
    }

    /**
     * @brief Finds the first point with at least the given number of pages.
     *
     * @param points Measured benchmark points.
     * @param pages Required page count.
     * @return Index of the first matching point, or points.size() if not found.
     */
    static size_t first_index_with_at_least_pages(const std::vector<shared_types::TlbSummaryPoint>& points,
                                                  size_t pages) {
        for (size_t i = 0; i < points.size(); ++i) {
            if (points[i].pages >= pages) {
                return i;
            }
        }

        return points.size();
    }

    /**
     * @brief Finds the first visible latency jump.
     *
     * The jump must be large enough in cycles and also large enough as a ratio.
     *
     * @param points Measured benchmark points.
     * @param start_index Index where search starts.
     * @param reference_level Baseline latency level.
     * @param ratio_threshold Minimum relative growth.
     * @param min_jump_cycles Minimum absolute jump in cycles per access.
     * @return Index of the jump, or std::nullopt if no jump is found.
     */
    static std::optional<size_t> find_latency_jump(const std::vector<shared_types::TlbSummaryPoint>& points,
                                                   size_t start_index, double reference_level, double ratio_threshold,
                                                   double min_jump_cycles) {
        if (start_index >= points.size()) {
            return std::nullopt;
        }

        const double safe_reference = std::max(reference_level, 0.001);
        const size_t first = std::max<size_t>(1, start_index);

        for (size_t i = first; i < points.size(); ++i) {
            const double previous = points[i - 1].median_cycles_per_access;
            const double current = points[i].median_cycles_per_access;

            const double absolute_jump = current - previous;
            const double local_ratio = previous > 0.0 ? current / previous : 0.0;
            const double global_ratio = current / safe_reference;

            if (absolute_jump < min_jump_cycles) {
                continue;
            }

            if (local_ratio < ratio_threshold || global_ratio < ratio_threshold) {
                continue;
            }

            if (!jump_is_sustained(points, i)) {
                continue;
            }

            return i;
        }

        return std::nullopt;
    }

    /**
     * @brief Checks that a latency jump is not only noise.
     *
     * The next point must not be much lower than the current point.
     *
     * @param points Measured benchmark points.
     * @param index Index of the possible jump.
     * @return true if the jump looks stable, false otherwise.
     */
    static bool jump_is_sustained(const std::vector<shared_types::TlbSummaryPoint>& points, size_t index) {
        if (index + 1 >= points.size()) {
            return true;
        }

        const double current = points[index].median_cycles_per_access;
        const double next = points[index + 1].median_cycles_per_access;

        return next >= current * 0.97;
    }

    /**
     * @brief Calculates the median of sorted values.
     *
     * The input vector must already be sorted.
     *
     * @param sorted_values Sorted values.
     * @return Median value.
     */
    static double median(const std::vector<double>& sorted_values) {
        const size_t size = sorted_values.size();

        if ((size % 2U) == 0U) {
            return 0.5 * (sorted_values[size / 2U - 1U] + sorted_values[size / 2U]);
        }

        return sorted_values[size / 2U];
    }
};

}  // namespace silicon_probe::tlb
