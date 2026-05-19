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

/**
 * @brief Measures TLB size.
 *
 * This benchmark touches more and more pages.
 * It looks for stable jumps in access time.
 */
class TlbMeasurer final : public core::Measurer {
   public:
    static constexpr size_t kMinPages            = 1;
    static constexpr size_t kDefaultMaxPages     = 4096;
    static constexpr size_t kPagesStep           = 2;
    static constexpr size_t kDefaultIterations   = 2'000'000;
    static constexpr size_t kRepeats             = 9;
    static constexpr size_t kWarmupRounds        = 2;
    static constexpr size_t kDefaultPageSize     = 4 * 1024;
    static constexpr size_t kHugePageSize        = 2 * 1024 * 1024;
    static constexpr size_t kPagePoolScale       = 8;
    static constexpr unsigned int kSeed          = 0xC0FFEEU;
    static constexpr size_t kMinL1CandidatePages = 64;
    static constexpr double kL1GrowthRatio       = 1.10;
    static constexpr double kL2GrowthRatio       = 1.20;
    static constexpr double kL1MinJumpCycles     = 0.50;
    static constexpr double kL2MinJumpCycles     = 1.00;
    static constexpr size_t kL2SearchGapPoints   = 2;
    static constexpr bool kLockMemory            = false;
    static constexpr bool kDisableThpFor4KPages  = true;

    /** @brief Settings for the TLB benchmark. */
    struct Config {
        bool enabled        = true;                           ///< Turn this measurer on or off.
        size_t max_pages    = kDefaultMaxPages;               ///< Largest page count to test.
        size_t iterations   = kDefaultIterations;             ///< Number of pointer steps in one sample.
        bool use_huge_pages = false;                          ///< Use huge pages instead of 4 KiB pages.
        platform::MeasurementEnvironmentOptions environment;  ///< CPU and scheduler settings for the run.
    };

    /** @brief Builds the measurer with default settings. */
    TlbMeasurer();

    /**
     * @brief Builds the measurer with custom settings.
     * @param config Settings for the next runs.
     */
    explicit TlbMeasurer(Config config);

    /**
     * @brief Returns the benchmark name.
     * @return Name used in logs and output.
     */
    std::string_view name() const noexcept override;

    /**
     * @brief Runs the TLB benchmark.
     * @param data Output data. The function writes TLB sizes here.
     */
    void measure(shared_types::CpuInfoData& data) override;

   private:
    /** @brief One node stored on a page. */
    struct PageNode {
        PageNode* next    = nullptr;  ///< Next page in the ring.
        std::uint64_t tag = 0;        ///< Value used to touch the page.
    } __attribute__((aligned(64)));

    /** @brief Owns the mapped memory used by the benchmark. */
    struct Mapping {
        void* base        = nullptr;  ///< Base address of the mapping.
        size_t size_bytes = 0;        ///< Mapping size in bytes.
        bool huge         = false;    ///< `true` if huge pages were used.
        bool locked       = false;    ///< `true` if mlock worked.

        Mapping()                          = default;
        Mapping(const Mapping&)            = delete;
        Mapping& operator=(const Mapping&) = delete;
        /**
         * @brief Moves mapping ownership from another object.
         * @param other Source object.
         */
        Mapping(Mapping&& other) noexcept;
        /**
         * @brief Replaces this mapping with a moved mapping.
         * @param other Source object.
         * @return Reference to this object.
         */
        Mapping& operator=(Mapping&& other) noexcept;
        /**
         * @brief Releases the owned mapping.
         */
        ~Mapping();
        /**
         * @brief Releases the mapping and clears the fields.
         */
        void release() noexcept;
    };

    /** @brief Rough jump positions in the TLB sweep. */
    struct Boundaries {
        std::optional<size_t> l1;  ///< First jump for L1 TLB.
        std::optional<size_t> l2;  ///< First jump for L2 TLB.
    };

    Config config_;

    /**
     * @brief Stops the compiler from removing test state.
     * @tparam T Value type.
     * @param value Value kept visible across the barrier.
     */
    template <typename T>
    static void compiler_barrier(const T& value);

    /**
     * @brief Returns the page size for this run.
     * @return 4 KiB or huge-page size.
     */
    size_t page_size_bytes() const noexcept;

    /**
     * @brief Checks that the settings are valid.
     */
    void validate_config() const;

    /**
     * @brief Multiplies two sizes with overflow check.
     * @param lhs Left value.
     * @param rhs Right value.
     * @return Product of the two values.
     */
    static size_t checked_mul(size_t lhs, size_t rhs);

    /**
     * @brief Builds the page counts for the sweep.
     * @return Test points from 1 page up to Config::max_pages.
     */
    std::vector<size_t> build_page_counts() const;

    /**
     * @brief Allocates the page pool.
     * @param size_bytes Total mapping size in bytes.
     * @return Owned mapping for the benchmark.
     */
    Mapping allocate_mapping(size_t size_bytes) const;

    /**
     * @brief Applies extra memory hints after allocation.
     * @param mapping Mapping to update.
     */
    void advise_mapping(const Mapping& mapping) const;

    /**
     * @brief Places one node on each page.
     * @param base Base address of the page pool.
     * @param page_count Number of pages in the pool.
     * @param page_size Page size in bytes.
     * @param cache_line_bytes Cache line size in bytes.
     * @return Node pointers that can be shuffled for the test.
     */
    static std::vector<PageNode*> make_page_nodes(
        void* base, size_t page_count, size_t page_size, size_t cache_line_bytes
    );

    /**
     * @brief Touches every node before timing.
     * @param nodes Nodes to pre-fault and init.
     */
    static void pretouch(const std::vector<PageNode*>& nodes);

    /**
     * @brief Links the first nodes into a ring.
     * @param order Ordered node list for the next test.
     * @param count Number of nodes to use.
     */
    static void link_ring(const std::vector<PageNode*>& order, size_t count);

    /**
     * @brief Warms code and branch state.
     * @param start First node in the warm-up ring.
     */
    void warm_instruction_path(PageNode* start);

    /**
     * @brief Runs a short warm-up pointer chase.
     * @param start First node in the ring.
     * @param pages Number of pages in the test.
     */
    static void warmup(PageNode* start, size_t pages);

    /**
     * @brief Measures one pointer-chase run.
     * @param start First node in the ring.
     * @return Total cycles for Config::iterations accesses.
     */
    std::uint64_t measure_cycles(PageNode* start) const;

    /**
     * @brief Measures one page-count point.
     * @param pool Full node pool.
     * @param order Temp storage for the selected pages.
     * @param rng Random generator for page order.
     * @param pages Number of pages in this test.
     * @param page_size Active page size in bytes.
     * @return Summary result for this point.
     */
    shared_types::TlbSummaryPoint measure_point(
        std::vector<PageNode*>& pool, std::vector<PageNode*>& order, std::mt19937& rng, size_t pages, size_t page_size
    ) const;
    /**
     * @brief Finds jump points in the sweep.
     * @param points Measured summary points.
     * @return Rough L1 and L2 jump indices.
     */
    static Boundaries detect_boundaries(const std::vector<shared_types::TlbSummaryPoint>& points);

    /**
     * @brief Computes the base level from the first points.
     * @param points Measured summary points.
     * @param max_count Max number of first points to use.
     * @return Base median cycles-per-access value.
     */
    static double mean_first_points(const std::vector<shared_types::TlbSummaryPoint>& points, size_t max_count);

    /**
     * @brief Finds the first point with at least @p pages.
     * @param points Measured summary points.
     * @param pages Smallest page count to accept.
     * @return Index of the first match, or `points.size()`.
     */
    static size_t first_index_with_at_least_pages(
        const std::vector<shared_types::TlbSummaryPoint>& points, size_t pages
    );

    /**
     * @brief Finds the first stable time jump after @p start_index.
     * @param points Measured summary points.
     * @param start_index First index to check.
     * @param reference_level Base time level.
     * @param ratio_threshold Smallest allowed growth ratio.
     * @param min_jump_cycles Smallest allowed jump in cycles.
     * @return Jump index, or `std::nullopt` if no jump was found.
     */
    static std::optional<size_t> find_latency_jump(
        const std::vector<shared_types::TlbSummaryPoint>& points, size_t start_index, double reference_level,
        double ratio_threshold, double min_jump_cycles
    );
    /**
     * @brief Checks that a jump stays high on the next point.
     * @param points Measured summary points.
     * @param index Jump candidate index.
     * @return `true` if the next point stays high.
     */
    static bool jump_is_sustained(const std::vector<shared_types::TlbSummaryPoint>& points, size_t index);

    /**
     * @brief Computes the median of sorted values.
     * @param sorted_values Values sorted in ascending order.
     * @return Median value.
     */
    static double median(const std::vector<double>& sorted_values);
};

}  // namespace silicon_probe::tlb
