// measurement/tlb/tlb_measurer.cpp
#include "measurement/tlb/tlb_measurer.hpp"

#include <numeric>

namespace silicon_probe::tlb {

// Mapping move & destructor
TlbMeasurer::Mapping::Mapping(Mapping&& other) noexcept {
    *this = std::move(other);
}

TlbMeasurer::Mapping& TlbMeasurer::Mapping::operator=(Mapping&& other) noexcept {
    if (this != &other) {
        release();
        base = std::exchange(other.base, nullptr);
        size_bytes = std::exchange(other.size_bytes, 0);
        huge = std::exchange(other.huge, false);
        locked = std::exchange(other.locked, false);
    }
    return *this;
}

TlbMeasurer::Mapping::~Mapping() { release(); }

void TlbMeasurer::Mapping::release() noexcept {
    if (base == nullptr) return;
    if (locked) munlock(base, size_bytes);
    if (huge) platform::huge_free(base, size_bytes);
    else munmap(base, size_bytes);
    base = nullptr;
    size_bytes = 0;
    huge = false;
    locked = false;
}

// TlbMeasurer constructors & name
TlbMeasurer::TlbMeasurer() : TlbMeasurer(Config{}) {}

TlbMeasurer::TlbMeasurer(Config config) : config_(std::move(config)) {
    SPDLOG_INFO("[{}] configured: pages {}..{}, iterations={}, page_size={}, huge_pages={}", name(), kMinPages,
                config_.max_pages, config_.iterations, page_size_bytes(), config_.use_huge_pages);
}

std::string_view TlbMeasurer::name() const noexcept {
    return "tlb";
}

// Public measure
void TlbMeasurer::measure(shared_types::CpuInfoData& data) {
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
    if (boundaries.l1) data.tlb_l1_size = points[*boundaries.l1].pages;
    if (boundaries.l2) data.tlb_l2_size = points[*boundaries.l2].pages;

    SPDLOG_INFO("[{}] TLB benchmark complete", name());
}

// Private static helpers (implementations)
template <typename T>
void TlbMeasurer::compiler_barrier(const T& value) {
    asm volatile("" : : "r,m"(value) : "memory");
}

size_t TlbMeasurer::page_size_bytes() const noexcept {
    return config_.use_huge_pages ? kHugePageSize : kDefaultPageSize;
}

void TlbMeasurer::validate_config() const {
    if (config_.max_pages < kMinPages) {
        throw std::invalid_argument("TLB benchmark max_pages must be >= 1");
    }
    if (config_.iterations == 0) {
        throw std::invalid_argument("TLB benchmark iterations must be positive");
    }
}

size_t TlbMeasurer::checked_mul(size_t lhs, size_t rhs) {
    if (rhs != 0 && lhs > std::numeric_limits<size_t>::max() / rhs) {
        throw std::overflow_error("TLB benchmark size overflows size_t");
    }
    return lhs * rhs;
}

std::vector<size_t> TlbMeasurer::build_page_counts() const {
    std::vector<size_t> result;
    for (size_t pages = kMinPages; pages <= config_.max_pages; pages *= kPagesStep) {
        result.push_back(pages);
        if (pages > config_.max_pages / kPagesStep) break;
    }
    if (result.back() != config_.max_pages) result.push_back(config_.max_pages);
    return result;
}

TlbMeasurer::Mapping TlbMeasurer::allocate_mapping(size_t size_bytes) const {
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

void TlbMeasurer::advise_mapping(const Mapping& mapping) const {
    if (mapping.base == nullptr || mapping.huge) return;
#if defined(MADV_NOHUGEPAGE)
    if constexpr (kDisableThpFor4KPages) {
        if (madvise(mapping.base, mapping.size_bytes, MADV_NOHUGEPAGE) != 0) {
            SPDLOG_WARN("[{}] madvise(MADV_NOHUGEPAGE) failed: {}", name(), std::strerror(errno));
        }
    }
#endif
}

std::vector<TlbMeasurer::PageNode*> TlbMeasurer::make_page_nodes(void* base, size_t page_count, size_t page_size,
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

void TlbMeasurer::pretouch(const std::vector<PageNode*>& nodes) {
    volatile std::uint64_t checksum = 0;
    for (size_t i = 0; i < nodes.size(); ++i) {
        nodes[i]->tag = i;
        checksum ^= nodes[i]->tag;
    }
    compiler_barrier(checksum);
}

void TlbMeasurer::link_ring(const std::vector<PageNode*>& order, size_t count) {
    for (size_t i = 0; i + 1 < count; ++i) order[i]->next = order[i + 1];
    order[count - 1]->next = order[0];
}

void TlbMeasurer::warm_instruction_path(PageNode* start) {
    warmup(start, 1);
    static_cast<void>(measure_cycles(start));
    platform::arch::mfence();
    platform::arch::lfence();
}

void TlbMeasurer::warmup(PageNode* start, size_t pages) {
    PageNode* cursor = start;
    size_t accesses = std::max(pages, pages * kWarmupRounds);
    for (size_t i = 0; i < accesses; ++i) cursor = cursor->next;
    compiler_barrier(cursor);
    platform::arch::mfence();
    platform::arch::lfence();
}

__attribute__((noinline)) std::uint64_t TlbMeasurer::measure_cycles(PageNode* start) const {
    PageNode* cursor = start;
    compiler_barrier(cursor);
    const std::uint64_t begin = platform::arch::tick();
    size_t remaining = config_.iterations;
    while (remaining >= 8) {
        cursor = cursor->next; cursor = cursor->next; cursor = cursor->next; cursor = cursor->next;
        cursor = cursor->next; cursor = cursor->next; cursor = cursor->next; cursor = cursor->next;
        remaining -= 8;
    }
    while (remaining-- > 0) cursor = cursor->next;
    const std::uint64_t end = platform::arch::tick();
    compiler_barrier(cursor);
    return end - begin;
}

shared_types::TlbSummaryPoint TlbMeasurer::measure_point(std::vector<PageNode*>& pool, std::vector<PageNode*>& order,
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
    return shared_types::TlbSummaryPoint{
        pages, pages * page_size,
        sorted.front(), median(sorted),
        std::accumulate(samples.begin(), samples.end(), 0.0) / static_cast<double>(samples.size()),
        sorted.back()
    };
}

TlbMeasurer::Boundaries TlbMeasurer::detect_boundaries(const std::vector<shared_types::TlbSummaryPoint>& points) {
    Boundaries result;
    if (points.size() < 3) return result;

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

double TlbMeasurer::mean_first_points(const std::vector<shared_types::TlbSummaryPoint>& points, size_t max_count) {
    const size_t count = std::min(points.size(), max_count);
    double sum = 0.0;
    for (size_t i = 0; i < count; ++i) sum += points[i].median_cycles_per_access;
    return sum / static_cast<double>(count);
}

size_t TlbMeasurer::first_index_with_at_least_pages(const std::vector<shared_types::TlbSummaryPoint>& points,
                                                    size_t pages) {
    for (size_t i = 0; i < points.size(); ++i) if (points[i].pages >= pages) return i;
    return points.size();
}

std::optional<size_t> TlbMeasurer::find_latency_jump(const std::vector<shared_types::TlbSummaryPoint>& points,
                                                     size_t start_index, double reference_level,
                                                     double ratio_threshold, double min_jump_cycles) {
    if (start_index >= points.size()) return std::nullopt;
    const double safe_reference = std::max(reference_level, 0.001);
    const size_t first = std::max<size_t>(1, start_index);
    for (size_t i = first; i < points.size(); ++i) {
        const double previous = points[i - 1].median_cycles_per_access;
        const double current = points[i].median_cycles_per_access;
        const double absolute_jump = current - previous;
        const double local_ratio = previous > 0.0 ? current / previous : 0.0;
        const double global_ratio = current / safe_reference;
        if (absolute_jump < min_jump_cycles) continue;
        if (local_ratio < ratio_threshold || global_ratio < ratio_threshold) continue;
        if (!jump_is_sustained(points, i)) continue;
        return i;
    }
    return std::nullopt;
}

bool TlbMeasurer::jump_is_sustained(const std::vector<shared_types::TlbSummaryPoint>& points, size_t index) {
    if (index + 1 >= points.size()) return true;
    const double current = points[index].median_cycles_per_access;
    const double next = points[index + 1].median_cycles_per_access;
    return next >= current * 0.97;
}

double TlbMeasurer::median(const std::vector<double>& sorted_values) {
    const size_t size = sorted_values.size();
    if ((size % 2U) == 0U) return 0.5 * (sorted_values[size / 2U - 1U] + sorted_values[size / 2U]);
    return sorted_values[size / 2U];
}

}  // namespace silicon_probe::tlb
