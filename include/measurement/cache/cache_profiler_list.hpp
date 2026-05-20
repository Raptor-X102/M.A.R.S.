// measurement/cache/cache_profiler_list.hpp
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <memory>
#include <random>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "infra/logging.hpp"
#include "platform/arch.hpp"
#include "platform/os.hpp"

namespace silicon_probe::cache {

/**
 * @brief Owns and prepares the pointer-chasing list used by the cache benchmark.
 *
 * The class allocates one buffer, places one element on each cache-line slot,
 * shuffles the links, and can flush the data before a timed sample.
 */
class CacheProfilerList {
   public:
    /** @brief Memory backend used for the pointer list buffer. */
    enum class MemoryType {
        aligned,    ///< Regular aligned allocation.
        huge_page,  ///< Huge-page-backed allocation when supported.
    };

    /** @brief One node in the pointer-chasing ring. */
    struct Element {
        Element* next = nullptr;
    };

    /** @brief Releases the raw memory with the matching backend. */
    struct MemoryDeleter {
        MemoryType type = MemoryType::aligned;
        size_t size     = 0;

        void operator()(char* ptr) const;
    };

    /**
     * @brief Builds a reusable pointer-list buffer.
     * @param cache_line_size Distance between two active elements.
     * @param max_count Largest number of active elements that may be requested.
     * @param memory_type Allocation backend for the buffer.
     */
    CacheProfilerList(size_t cache_line_size, size_t max_count, MemoryType memory_type = MemoryType::aligned);
    ~CacheProfilerList();

    CacheProfilerList(const CacheProfilerList&)                = delete;
    CacheProfilerList& operator=(const CacheProfilerList&)     = delete;
    CacheProfilerList(CacheProfilerList&&) noexcept            = default;
    CacheProfilerList& operator=(CacheProfilerList&&) noexcept = default;

    /** @brief Returns the first element of the current ring. */
    Element* first() const noexcept;
    /** @brief Returns the current number of active elements. */
    size_t element_count() const noexcept;
    /** @brief Returns the configured cache-line stride in bytes. */
    size_t line_size() const noexcept;
    /** @brief Returns the total allocated size in bytes. */
    size_t total_size() const noexcept;
    /** @brief Tries to evict the active buffer from cache before timing. */
    void flush_from_cache() const;
    /**
     * @brief Rebuilds the active ring for a new test size.
     * @param count Number of active elements to use.
     * @param seed Random seed for the link order.
     */
    void prepare(size_t count, unsigned int seed);

   private:
    size_t line_size_       = 0;
    size_t element_count_   = 0;
    size_t allocated_count_ = 0;
    std::unique_ptr<char, MemoryDeleter> memory_;
    MemoryType memory_type_ = MemoryType::aligned;
    Element* first_element_ = nullptr;

    static std::string build_error_message(const std::string& prefix, size_t value);
    Element* element_at(size_t index) const noexcept;
    void allocate(size_t count);
    void verify_cycle() const;
};

}  // namespace silicon_probe::cache
